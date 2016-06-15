/** \file
*  	LedBurn protocol server for beagle bone black.
*	Receive udp packets with LedBurn protocol data, and sends it to ws281x led pixels
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <stdint.h>
#include <ctype.h>
#include <stdbool.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <fcntl.h>
#include "ledscape.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

#define MAX_SUPPORTED_PIXELS_PER_STRAND 1500
#define DEFAULT_MAX_PIXELS 600
#define LB_HEADER_SIZE (8+8+8)

int pixelsPerStrand = DEFAULT_MAX_PIXELS;

// we support up to 4096 segments, or 64 segments per strip if all strips are used, which is 10 pixels per packet.
// this is more than enough
#define MAX_SUPPORTED_SEGMENTS (LEDSCAPE_NUM_STRIPS * 64)
bool receivedSegArr[MAX_SUPPORTED_SEGMENTS] = {false};
uint32_t currentFrame = 0;
uint32_t numOfReceivedSegments = 0;

// framerate protection
bool fullFrameReady = false;

// LedScape things
ledscape_t *leds = NULL;
uint8_t buffer_index = 0;
ledscape_frame_t *frame = NULL;

typedef struct PacketHeaderData
{
  uint32_t frameId;
  uint32_t segInFrame;
  uint32_t currSegId;
  uint16_t stripId;
  uint16_t pixelId;
  uint16_t numOfPixels; // this is not actually header data, but it's nice to have it here
} PacketHeaderData;

void ChangeLedScapeBuffers()
{
	buffer_index = (buffer_index+1)%2;
	frame = ledscape_frame(leds, buffer_index);
}

void SendColorsToStrips()
{
	// Wait for previous send to complete if still in progress
	ledscape_wait(leds);
	
	// the following line is critical for the leds to have proper display.
	// if it is absent, the leds does not operate well if draw imidiately one after the other
	// I don't know why, but suspect it has to do with the ws2812 reset time
	// not being handled correctly by the pru code.
	// TODO: dig into the pru code and understand why
	usleep(1e2 /* 100us */);
	
	// Send the frame to the PRU
	ledscape_draw(leds, buffer_index);
	
	ChangeLedScapeBuffers();
	fullFrameReady = false;
}

void SetAllSameColor(uint8_t r, uint8_t g, uint8_t b) {
	for(int i=0; i<3; i++) {
		for(int s = 0; s < LEDSCAPE_NUM_STRIPS; s++) {		
			for(int i=0; i<pixelsPerStrand; i++)
			{
				ledscape_set_color(
					frame,
					COLOR_ORDER_BRG,
					s,
					i,
					r,
					g,
					b
				);
			}	
		}
		SendColorsToStrips();	
	}
}

void StartLedScape()
{
	printf("[main] Starting LEDscape...\n");

	leds = ledscape_init_with_programs(
		pixelsPerStrand,
		"pru/bin/ws281x-come-million-box-pru0.bin",
		"pru/bin/ws281x-come-million-box-pru1.bin"
	);		
	
	ChangeLedScapeBuffers();

	printf("[main] Done Starting LEDscape...\n");	
}

void ResetCounter(uint32_t newFrameId)
{
  	currentFrame = newFrameId;
  	numOfReceivedSegments = 0;
  	for(int i=0; i<MAX_SUPPORTED_SEGMENTS; i++) {
    	receivedSegArr[i] = false;  
	}
}

bool VerifyLedBurnPacket(const uint8_t packetBuf[], int packetSize)
{
  if(packetSize < LB_HEADER_SIZE)
    return false;
  if(memcmp(packetBuf, "LedBurn", 7) != 0)
    return false;  
  uint8_t protocolVersion = packetBuf[7];
  if(protocolVersion != 0)
    return false;
  int payloadLength = (packetSize - LB_HEADER_SIZE);
  if( (payloadLength % 3) != 0)
    return false;

  return true;
}

PacketHeaderData ParsePacketHeader(const uint8_t packetBuf[], int packetSize)
{
  PacketHeaderData phd;
  
  phd.frameId = (*((const uint32_t *) (packetBuf + 8) ));
  phd.segInFrame = (*((const uint32_t *) (packetBuf + 12) ));

  phd.currSegId = (*((const uint32_t *) (packetBuf + 16) ));
  phd.stripId = (*((const uint16_t *) (packetBuf + 20) ));
  phd.pixelId = (*((const uint16_t *) (packetBuf + 22) ));

  phd.numOfPixels = (packetSize - LB_HEADER_SIZE) / 3;

  return phd;
}

// return true if packet is ok.
// return false if packet should be ignored
bool BeforePaintLeds(const PacketHeaderData *phd)
{
  if(phd->segInFrame >= MAX_SUPPORTED_SEGMENTS || phd->currSegId >= phd->segInFrame)
    return false;
  
  // this is the common case with no packet losses
  if(phd->frameId == currentFrame)
    return true;
  
  // if the current frame is old. don't use it!
  // do the math with int64, to avoid overflows
  // unless it's very old, in which case, assume the sender restarted and use it
  int64_t diffFromCurrent = (int64_t)phd->frameId - (int64_t)currentFrame;
  if(diffFromCurrent > -500 && diffFromCurrent < 0) // 500 is 10 seconds in 50HZ
    return false;

  // if we are here, then this frame is not what we expected, but it is not frame from udp re-order.
  // so we change our reference point to it!
  printf("info: new frame reference point detected. old frame id: %u. new frame id: %u. diff: %" PRId64 "\n", currentFrame, phd->frameId, diffFromCurrent);
  ResetCounter(phd->frameId);
  SendColorsToStrips(); // use the leds we already recived
	SetAllSameColor(0, 0, 0);
  return true;
}

void PaintLeds(const uint8_t packetBuf[], const PacketHeaderData *phd)
{
	// avoid overrun the allowed buffer
	if(phd->stripId >= LEDSCAPE_NUM_STRIPS)
		return;
	if(phd->pixelId >= pixelsPerStrand)
		return;
	int numOfPixels = min(phd->numOfPixels, (uint16_t)(pixelsPerStrand - phd->pixelId) ); // pixelsPerStrand - phd.pixelId > 0

	const uint8_t *bufStartPointer = (const uint8_t *)packetBuf + LB_HEADER_SIZE;

	for(int i=0; i<numOfPixels; i++)
	{
		const uint8_t *pixelStartPointer = bufStartPointer + i * 3;
		ledscape_set_color(
			frame,
			COLOR_ORDER_BRG,
			phd->stripId,
			phd->pixelId + i,
			*(pixelStartPointer+0),
			*(pixelStartPointer+1),
			*(pixelStartPointer+2)
		);
	}
}

void AfterPaintLeds(const PacketHeaderData *phd)
{
  if(receivedSegArr[phd->currSegId])
  {
    // we already have this segment. this is a duplicate packet!
    return;
  }

  receivedSegArr[phd->currSegId] = true;
  numOfReceivedSegments++;

  if(numOfReceivedSegments >= phd->segInFrame)
  {
  	ResetCounter(currentFrame + 1);
  	fullFrameReady = true;
  }
}

void MainLoop()
{
	printf("Initialize udp listen socket\n");
	
	const int sock = socket(AF_INET6, SOCK_DGRAM, 0);
	if (sock < 0) {
		die("[udp] socket failed: %s\n", strerror(errno));
	}

	// set the socket to non bloking.
	int flags = fcntl(sock,F_GETFL);
	flags |= O_NONBLOCK;
	fcntl(sock, F_SETFL, flags);


	struct sockaddr_in6 addr;
	bzero(&addr, sizeof(addr));
	addr.sin6_family = AF_INET6;
	addr.sin6_addr = in6addr_any;
	addr.sin6_port = htons(2000);

	if (bind(sock, (const struct sockaddr*) &addr, sizeof(addr)) < 0)
	{
		die("[udp] bind port %d failed: %s\n", 2000, strerror(errno));
	}

	uint8_t buf[65536];
	printf("Done initializing udp listen socket\n");	
	
	
	printf("Starting main loop\n");
	ChangeLedScapeBuffers(); // this will initialize it as well

	for(;;) {
		
		const ssize_t rc = recv(sock, buf, sizeof(buf), 0);
		if(rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if(fullFrameReady == true && !is_ledscape_busy(leds)) {
		    	SendColorsToStrips();
		    }
			continue;			
		}
		if (rc < 0) {
			fprintf(stderr, "[udp] recv failed: %s\n", strerror(errno));
			continue;
		}
		
		if(!VerifyLedBurnPacket(buf, rc))
		{
			fprintf(stderr, "[udp] recv packet which is not of LedBurn protocol!\n");
			continue;
		}
		
		PacketHeaderData phd = ParsePacketHeader(buf, rc);
		if(!BeforePaintLeds(&phd))
		{
		  fprintf(stderr, "[udp] BeforePaintLeds failed!\n");
		  continue;
		}
		PaintLeds(buf, &phd);
		AfterPaintLeds(&phd);
	}

	ledscape_close(leds);
}

void PlayInitSequence() {
	SetAllSameColor(255, 0, 0);
	usleep(1000 * 1000);
	SetAllSameColor(0, 255, 0);
	usleep(1000 * 1000);
	SetAllSameColor(0, 0, 255);
	usleep(1000 * 1000);
	SetAllSameColor(0, 0, 0);
}

void SetNumberOfPixelsInStrand(int argc, char ** argv) {
	if(argc > 1) {
		char *endPtr;
		errno = 0; /* To distinguish success/failure after call */
		long numberOfPixels = strtol(argv[1], &endPtr, 10);\
		
		// check non integer values
		if(endPtr == argv[1] || *endPtr != '\0') {
			fprintf(stderr, "first parameter to the ledburn server should be number of pixels. received non integer value: '%s'\n", argv[1]);
			exit(EXIT_FAILURE);
		}
		
		// check out of range
		if ((errno == ERANGE && (numberOfPixels == LONG_MAX || numberOfPixels == LONG_MIN)) || (errno != 0 && numberOfPixels == 0)) {
			fprintf(stderr, "first parameter to the ledburn server should be number of pixels. received invalid value: %s\n", argv[1]);
			exit(EXIT_FAILURE);
		}
		
		// check if value is not reasonable

		if(numberOfPixels > MAX_SUPPORTED_PIXELS_PER_STRAND || numberOfPixels <= 0) {
			fprintf(stderr, "number of pixels from command line argument is not supported. value should be between [1, %d]. received: %ld\n", MAX_SUPPORTED_PIXELS_PER_STRAND, numberOfPixels);
			exit(EXIT_FAILURE);			
		}
		
		pixelsPerStrand = numberOfPixels;
		printf("pixels per strand set from command line argument to = %d\n", pixelsPerStrand);
	}
	else {
		printf("pixels per strand not set from command line argument. using default value %d\n", pixelsPerStrand);
	}
}

int main(int argc, char ** argv)
{
	SetNumberOfPixelsInStrand(argc, argv);
	StartLedScape();
	PlayInitSequence();
	MainLoop();
}
