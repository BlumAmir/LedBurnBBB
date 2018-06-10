/** \file
*  	LedBurn protocol server for beagle bone black.
*	Receive udp packets with LedBurn protocol data, and sends it to ws281x led pixels
*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
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
#include "ledscape.h"

#define min(a, b) ((a) < (b) ? (a) : (b))

#define MAX_PIXELS 600
#define LB_HEADER_SIZE (8+8+8)

// we support up to 4096 segments, or 64 segments per strip if all strips are used, which is 10 pixels per packet.
// this is more than enough
#define MAX_SUPPORTED_SEGMENTS (LEDSCAPE_NUM_STRIPS * 64)
bool receivedSegArr[MAX_SUPPORTED_SEGMENTS] = {false};
uint32_t currentFrame = 0;
uint32_t numOfReceivedSegments = 0;

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

void StartLedScape()
{
	printf("[main] Starting LEDscape...\n");

	leds = ledscape_init_with_programs(
		MAX_PIXELS,
		"pru/bin/ws281x-rgb-123-v3-pru0.bin",
		"pru/bin/ws281x-rgb-123-v3-pru1.bin"
	);		

	printf("[main] Done Starting LEDscape...\n");	
}

void ResetCounter(uint32_t newFrameId)
{
  currentFrame = newFrameId;
  numOfReceivedSegments = 0;
  for(int i=0; i<MAX_SUPPORTED_SEGMENTS; i++)
    receivedSegArr[i] = false;  
}

void ChangeLedScapeBuffers()
{
	buffer_index = (buffer_index+1)%2;
	frame = ledscape_frame(leds, buffer_index);
}

void SendColorsToStrips()
{
	// Wait for previous send to complete if still in progress
	ledscape_wait(leds);
	// Send the frame to the PRU
	ledscape_draw(leds, buffer_index);
	
	ChangeLedScapeBuffers();
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
  ResetCounter(phd->frameId);
  SendColorsToStrips(); // use the leds we already recived
  return true;
}

void PaintLeds(const uint8_t packetBuf[], const PacketHeaderData *phd)
{
	// avoid overrun the allowed buffer
	if(phd->stripId >= LEDSCAPE_NUM_STRIPS)
		return;
	if(phd->pixelId >= MAX_PIXELS)
		return;
	int numOfPixels = min(phd->numOfPixels, (uint16_t)(MAX_PIXELS - phd->pixelId) ); // MAX_PIXELS - phd.pixelId > 0

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
    ResetCounter(phd->frameId + 1);
    SendColorsToStrips();
  }
}

void MainLoop()
{
	printf("Initialize udp listen socket\n");
	
	const int sock = socket(AF_INET6, SOCK_DGRAM, 0);

	if (sock < 0)
		die("[udp] socket failed: %s\n", strerror(errno));

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

int main(int argc, char ** argv)
{
	argc = argc; argv = argv;
	StartLedScape();
	MainLoop();
}