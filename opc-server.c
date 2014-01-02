/** \file
 *  OPC image packet receiver.
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <fcntl.h>
#include <termios.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <inttypes.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <getopt.h>
#include "util.h"
#include "ledscape.h"

#include <pthread.h>

// TODO:
// Server:
// 	- ip-stack Agnostic socket stuff
//  - UDP receiver
// Config:
//  - White-balance, curve adjustment
//  - Respecting interpolation and dithering settings


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// DEFINES
#define TRUE 1
#define FALSE 0

#define min(a, b) ((a) < (b) ? (a) : (b))
#define max(a, b) ((a) > (b) ? (a) : (b))

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Method declarations

// Frame Manipulation
void ensure_frame_data();
void set_next_frame_data(uint8_t* frame_data, uint32_t data_size);
void rotate_frames();

// Threads
void* render_thread(void* threadarg);
void* udp_server_thread(void* threadarg);
void* tcp_server_thread(void* threadarg);

// Config Methods
void build_lookup_tables();

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Global Data
static struct {
	ledscape_output_mode_t pru0_mode;
	ledscape_output_mode_t pru1_mode;

	uint16_t tcp_port;
	uint16_t udp_port;
	uint16_t leds_per_strip;

	uint8_t interpolation_enabled;
	uint8_t dithering_enabled;
	uint8_t lut_enabled;

	struct {
		float red;
		float green;
		float blue;
	} white_point;

	float lum_power;

	pthread_mutex_t mutex;
	char json[4096];
} g_server_config = {
	.pru0_mode = WS281x,
	.pru1_mode = WS281x,
	.tcp_port = 7890,
	.udp_port = 7890,
	.leds_per_strip = 176,
	.interpolation_enabled = TRUE,
	.dithering_enabled = TRUE,
	.lut_enabled = TRUE,
	.white_point = { .9, 1, 1},
	.lum_power = 2,
	.mutex = PTHREAD_MUTEX_INITIALIZER
};

typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} __attribute__((__packed__)) buffer_pixel_t;

typedef struct {
	int8_t r;
	int8_t g;
	int8_t b;

	int8_t last_effect_frame_r;
	int8_t last_effect_frame_g;
	int8_t last_effect_frame_b;
} __attribute__((__packed__)) pixel_delta_t;

static struct
{
	buffer_pixel_t* previous_frame_data;
	buffer_pixel_t* current_frame_data;
	buffer_pixel_t* next_frame_data;

	pixel_delta_t* frame_dithering_overflow;

	uint8_t has_next_frame;

	uint32_t frame_size;

	uint64_t frame_count;

	struct timeval previous_frame_tv;
	struct timeval current_frame_tv;
	struct timeval next_frame_tv;

	struct timeval prev_current_delta_tv;

	ledscape_t * leds;

	uint32_t red_lookup[257];
	uint32_t green_lookup[257];
	uint32_t blue_lookup[257];

	pthread_mutex_t mutex;
} g_frame_data = {
	.previous_frame_data = (buffer_pixel_t*)NULL,
	.current_frame_data = (buffer_pixel_t*)NULL,
	.next_frame_data = (buffer_pixel_t*)NULL,
	.has_next_frame = FALSE,
	.frame_dithering_overflow = (buffer_pixel_t*)NULL,
	.frame_size = 0,
	.frame_count = 0,
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.leds = NULL
};

static struct
{
	pthread_t render_thread;
	pthread_t tcp_server_thread;
	pthread_t udp_server_thread;
} g_threads;


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// main()
static struct option long_options[] =
{
    {"tcp-port", optional_argument, NULL, 'p'},
    {"udp-port", optional_argument, NULL, 'P'},
    {"count", optional_argument, NULL, 'c'},
    {"dimensions", optional_argument, NULL, 'd'},

    {"no-interpolation", no_argument, NULL, 'i'},
    {"no-dithering", no_argument, NULL, 't'},
    {"no-lut", no_argument, NULL, 'l'},

    {"lum_power", optional_argument, NULL, 'L'},

    {"red_bal", optional_argument, NULL, 'r'},
    {"green_bal", optional_argument, NULL, 'g'},
    {"blue_bal", optional_argument, NULL, 'b'},

    {"pru0_mode", optional_argument, NULL, '0'},
    {"pru1_mode", optional_argument, NULL, '1'},

    {NULL, 0, NULL, 0}
};

int main(int argc, char ** argv)
{
	extern char *optarg;
	int opt;
	while ((opt = getopt_long(argc, argv, "p:P:c:d:itlL:r:g:b:0:1:", long_options, NULL)) != -1)
	{
		switch (opt)
		{
		case 'p': {
			g_server_config.tcp_port = atoi(optarg);
		} break;

		case 'P': {
			g_server_config.udp_port = atoi(optarg);
		} break;

		case 'c': {
			g_server_config.leds_per_strip = atoi(optarg);
		} break;

		case 'd': {
			int width=0, height=0;

			if (sscanf(optarg,"%dx%d", &width, &height) == 2) {
				g_server_config.leds_per_strip = width * height;
			} else {
				printf("Invalid argument for -d; expected NxN; actual: %s", optarg);
				exit(EXIT_FAILURE);
			}
		} break;


		case 'i': {
			g_server_config.interpolation_enabled = FALSE;
		} break;

		case 't': {
			g_server_config.dithering_enabled = FALSE;
		} break;

		case 'l': {
			g_server_config.lut_enabled = FALSE;
		} break;

		case 'L': {
			g_server_config.lum_power = atof(optarg);
		} break;

		case 'r': {
			g_server_config.white_point.red = atof(optarg);
		} break;

		case 'g': {
			g_server_config.white_point.green = atof(optarg);
		} break;

		case 'b': {
			g_server_config.white_point.blue = atof(optarg);
		} break;

		case '0': {
			g_server_config.pru0_mode = ledscape_output_mode_from_string(optarg);
		} break;

		case '1': {
			g_server_config.pru1_mode = ledscape_output_mode_from_string(optarg);
		} break;

		default:
			fprintf(stderr, "Usage: %s [-p <port>] [-c <led_count> | -d <width>x<height>] [-i | --no-interpolation] "
				"[-t | --no-dithering] [-l | --no-lut] [-L | lum_power <lum_power>] "
				"[-r | -red_bal <red_bal>] [-g | -green_bal <green_bal>] [-b | -blue_bal <blue_bal>]\n", argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	// largest possible UDP packet
	if (g_server_config.leds_per_strip*LEDSCAPE_NUM_STRIPS*3 >= 65536) {
		die("[main] %u pixels cannot fit in a UDP packet.\n", g_server_config.leds_per_strip);
	}

	// Init LEDscape
	g_frame_data.leds = ledscape_init_with_modes(
		g_server_config.leds_per_strip,
		g_server_config.pru0_mode,
		g_server_config.pru1_mode
	);

	// Build config JSON
	sprintf(
		g_server_config.json,
		"{\n"
			"\t" "\"pru0Mode\": \"%s\"," "\n"
			"\t" "\"pru1Mode\": \"%s\"," "\n"
			"\t" "\"ledsPerStrip\": %d," "\n"

			"\t" "\"tcpPort\": %d," "\n"
			"\t" "\"udpPort\": %d," "\n"

			"\t" "\"enableInterpolation\": %s," "\n"
			"\t" "\"enableDithering\": %s," "\n"
			"\t" "\"enableLookupTable\": %s," "\n"

			"\t" "\"lumCurvePower\": %.4f," "\n"
			"\t" "\"whitePoint\": {" "\n"
			"\t\t" "\"red\": %.4f," "\n"
			"\t\t" "\"green\": %.4f," "\n"
			"\t\t" "\"blue\": %.4f" "\n"
			"\t" "}" "\n"
		"}\n",

		ledscape_output_mode_to_string(g_frame_data.leds->pru0_mode),
		ledscape_output_mode_to_string(g_frame_data.leds->pru1_mode),

		g_server_config.leds_per_strip,

		g_server_config.tcp_port,
		g_server_config.udp_port,

		g_server_config.interpolation_enabled ? "true" : "false",
		g_server_config.dithering_enabled ? "true" : "false",
		g_server_config.lut_enabled ? "true" : "false",

		(double)g_server_config.lum_power,
		(double)g_server_config.white_point.red,
		(double)g_server_config.white_point.green,
		(double)g_server_config.white_point.blue
	);

	fprintf(stderr,
		"[main] Starting server on ports (tcp=%d, udp=%d) for %d pixels on %d strips\n",
		g_server_config.tcp_port, g_server_config.udp_port, g_server_config.leds_per_strip, LEDSCAPE_NUM_STRIPS
	);
	fprintf(stderr, g_server_config.json);

	build_lookup_tables();
	ensure_frame_data();

	pthread_create(&g_threads.render_thread, NULL, render_thread, NULL);
	pthread_create(&g_threads.udp_server_thread, NULL, udp_server_thread, NULL);
	pthread_create(&g_threads.tcp_server_thread, NULL, tcp_server_thread, NULL);


	pthread_exit(NULL);
}

void build_lookup_tables() {
	pthread_mutex_lock(&g_frame_data.mutex);
	pthread_mutex_lock(&g_server_config.mutex);

	float white_points[] = {
		g_server_config.white_point.red,
		g_server_config.white_point.green,
		g_server_config.white_point.blue
	};

	uint16_t* lookup_tables[] = {
		g_frame_data.red_lookup,
		g_frame_data.green_lookup,
		g_frame_data.blue_lookup
	};

	for (uint16_t c=0; c<3; c++) {
		for (uint16_t i=0; i<257; i++) {
			double normalI = (double)i / 256;
			normalI *= white_points[c];

			double output = pow(normalI, g_server_config.lum_power);
			int64_t longOutput = (output * 0xFFFF) + 0.5;
			int32_t clampedOutput = max(0, min(0xFFFF, longOutput));

			lookup_tables[c][i] = clampedOutput;
		}
	}

	pthread_mutex_unlock(&g_server_config.mutex);
	pthread_mutex_unlock(&g_frame_data.mutex);
}

/**
 * Ensure that the frame buffers are allocated to the correct values.
 */
void ensure_frame_data() {
	pthread_mutex_lock(&g_server_config.mutex);
	uint32_t led_count = g_server_config.leds_per_strip * LEDSCAPE_NUM_STRIPS;
	pthread_mutex_unlock(&g_server_config.mutex);

	pthread_mutex_lock(&g_frame_data.mutex);
	if (g_frame_data.frame_size != led_count) {
		fprintf(stderr, "Allocating buffers for %d pixels (%d bytes)\n", led_count, led_count * 3 /*channels*/ * 4 /*buffers*/ * sizeof(uint16_t));

		if (g_frame_data.previous_frame_data != NULL) {
			free(g_frame_data.previous_frame_data);
			free(g_frame_data.current_frame_data);
			free(g_frame_data.next_frame_data);
			free(g_frame_data.frame_dithering_overflow);
		}

		g_frame_data.frame_size = led_count;
		g_frame_data.previous_frame_data = malloc(led_count * sizeof(buffer_pixel_t));
		g_frame_data.current_frame_data = malloc(led_count * sizeof(buffer_pixel_t));
		g_frame_data.next_frame_data = malloc(led_count * sizeof(buffer_pixel_t));
		g_frame_data.frame_dithering_overflow = malloc(led_count * sizeof(pixel_delta_t));
		g_frame_data.frame_count = 0;
		g_frame_data.has_next_frame = FALSE;
		printf("frame_size1=%u\n", g_frame_data.frame_size);
	}
	pthread_mutex_unlock(&g_frame_data.mutex);
}

/**
 * Set the next frame of data to the given 8-bit RGB buffer after rotating the buffers.
 */
void set_next_frame_data(uint8_t* frame_data, uint32_t data_size) {
	rotate_frames();

	pthread_mutex_lock(&g_frame_data.mutex);

	// Prevent buffer overruns
	data_size = min(data_size, g_frame_data.frame_size * 3);

	memcpy(g_frame_data.next_frame_data, frame_data, data_size);
	memset((uint8_t*)g_frame_data.next_frame_data + data_size, 0, (g_frame_data.frame_size*3 - data_size));

	// Update the timestamp & count
	gettimeofday(&g_frame_data.next_frame_tv, NULL);
	g_frame_data.frame_count ++;

	g_frame_data.has_next_frame = (g_frame_data.frame_count > 2);

	pthread_mutex_unlock(&g_frame_data.mutex);
}

/**
 * Rotate the buffers, dropping the previous frame and loading in the new one
 */
void rotate_frames() {
	pthread_mutex_lock(&g_frame_data.mutex);

	// Update timestamps
	g_frame_data.previous_frame_tv = g_frame_data.current_frame_tv;
	g_frame_data.current_frame_tv = g_frame_data.next_frame_tv;

	// Copy data
	buffer_pixel_t* temp = g_frame_data.previous_frame_data;
	g_frame_data.previous_frame_data = g_frame_data.current_frame_data;
	g_frame_data.current_frame_data = g_frame_data.next_frame_data;
	g_frame_data.next_frame_data = temp;

	g_frame_data.has_next_frame = FALSE;

	// Update the delta time stamp
	timersub(&g_frame_data.current_frame_tv, &g_frame_data.previous_frame_tv, &g_frame_data.prev_current_delta_tv);

	pthread_mutex_unlock(&g_frame_data.mutex);
}

inline uint16_t lutInterpolate(uint16_t value, uint16_t* lut) {
	// Inspired by FadeCandy: https://github.com/scanlime/fadecandy/blob/master/firmware/fc_pixel_lut.cpp

	uint16_t index = value >> 8; // Range [0, 0xFF]
	uint16_t alpha = value & 0xFF; // Range [0, 0xFF]
	uint16_t invAlpha = 0x100 - alpha; // Range [1, 0x100]

	// Result in range [0, 0xFFFF]
	return (lut[index] * invAlpha + lut[index + 1] * alpha) >> 8;
}

void* render_thread(void* unused_data)
{
	unused_data=unused_data; // Suppress Warnings
	fprintf(stderr, "[render] Starting render thread for %u total pixels\n", g_server_config.leds_per_strip * LEDSCAPE_NUM_STRIPS);

	// Timing Variables
	struct timeval frame_progress_tv, now_tv;
	uint16_t frame_progress16, inv_frame_progress16;

	const unsigned report_interval = 1;
	unsigned last_report = 0;
	unsigned long delta_sum = 0;
	unsigned frames = 0;
	uint32_t delta_avg = 2000;

	uint8_t buffer_index = 0;
	int8_t ditheringFrame = 0;
	for(;;) {
		pthread_mutex_lock(&g_frame_data.mutex);

		// Skip frames if there isn't enough data
		if (g_frame_data.frame_count < 3) {
			usleep(2e6);
			printf("[render] Awaiting sufficient data...\n");
			pthread_mutex_unlock(&g_frame_data.mutex);
			continue;
		}


		// Calculate the time delta and current percentage (as a 16-bit value)
		gettimeofday(&now_tv, NULL);
		timersub(&now_tv, &g_frame_data.next_frame_tv, &frame_progress_tv);

		// Calculate current frame and previous frame time
		uint64_t frame_progress_us = frame_progress_tv.tv_sec*1000000 + frame_progress_tv.tv_usec;
		uint64_t last_frame_time_us = g_frame_data.prev_current_delta_tv.tv_sec*1000000 + g_frame_data.prev_current_delta_tv.tv_usec;

		// Check for current frame exhaustion
		if (frame_progress_us > last_frame_time_us) {
			uint8_t has_next_frame = g_frame_data.has_next_frame;
			pthread_mutex_unlock(&g_frame_data.mutex);

			if (has_next_frame) {
			// If we have more data, rotate it in.
				rotate_frames();
			} else {
				// Otherwise sleep for a moment and wait for more data
				usleep(1000);
			}

			continue;
		}

		frame_progress16 = (frame_progress_us << 16) / last_frame_time_us;
		inv_frame_progress16 = 0x10000 - frame_progress16;

		if (frame_progress_tv.tv_sec > 5) {
			printf("[render] No data for 5 seconds; suspending render thread.\n");
			g_frame_data.frame_count = 0;
			pthread_mutex_unlock(&g_frame_data.mutex);
			continue;
		}

		// printf("%d of %d (%d)\n",
		// 	(frame_progress_tv.tv_sec*1000000 + frame_progress_tv.tv_usec) ,
		// 	(g_frame_data.prev_current_delta_tv.tv_sec*1000000 + g_frame_data.prev_current_delta_tv.tv_usec),
		// 	frame_progress16
		// );

		// Setup LEDscape for this frame
		buffer_index = (buffer_index+1)%2;
		ledscape_frame_t * const frame = ledscape_frame(g_frame_data.leds, buffer_index);

		// Build the render frame
		uint16_t led_count = g_frame_data.frame_size;
		uint16_t leds_per_strip = led_count / LEDSCAPE_NUM_STRIPS;
		uint32_t data_index = 0;

		// Update the dithering frame counter
		ditheringFrame ++;

		// Timing stuff
		struct timeval start_tv, stop_tv, delta_tv;
		gettimeofday(&start_tv, NULL);

		// Check the server config for dithering and interpolation options
		pthread_mutex_lock(&g_server_config.mutex);

		// Only enable dithering if we're better than 100fps
		uint8_t dithering_enabled = (delta_avg < 10000) && g_server_config.dithering_enabled;
		uint8_t interpolation_enabled = g_server_config.interpolation_enabled;
		uint8_t lut_enabled = g_server_config.lut_enabled;
		pthread_mutex_unlock(&g_server_config.mutex);

		// Only allow dithering to take effect if it blinks faster than 60fps
		uint32_t maxDitherFrames = 16667 / delta_avg;

		for (uint32_t strip_index=0; strip_index<LEDSCAPE_NUM_STRIPS; strip_index++) {
			for (uint32_t led_index=0; led_index<leds_per_strip; led_index++, data_index++) {
				buffer_pixel_t* pixel_in_prev = &g_frame_data.previous_frame_data[data_index];
				buffer_pixel_t* pixel_in_current = &g_frame_data.current_frame_data[data_index];
				pixel_delta_t* pixel_in_overflow = &g_frame_data.frame_dithering_overflow[data_index];

				ledscape_pixel_t* const pixel_out = & frame[led_index].strip[strip_index];

				int32_t interpolatedR;
				int32_t interpolatedG;
				int32_t interpolatedB;

				// Interpolate
				if (interpolation_enabled) {
					interpolatedR = (pixel_in_prev->r*inv_frame_progress16 + pixel_in_current->r*frame_progress16) >> 8;
					interpolatedG = (pixel_in_prev->g*inv_frame_progress16 + pixel_in_current->g*frame_progress16) >> 8;
					interpolatedB = (pixel_in_prev->b*inv_frame_progress16 + pixel_in_current->b*frame_progress16) >> 8;
				} else {
					interpolatedR = pixel_in_current->r << 8;
					interpolatedG = pixel_in_current->g << 8;
					interpolatedB = pixel_in_current->b << 8;
				}

				// Apply LUT
				if (lut_enabled) {
					interpolatedR = lutInterpolate(interpolatedR, g_frame_data.red_lookup);
					interpolatedG = lutInterpolate(interpolatedG, g_frame_data.green_lookup);
					interpolatedB = lutInterpolate(interpolatedB, g_frame_data.blue_lookup);
				}

				// Reset dithering for this pixel if it's been too long since it actually changed anything. This serves to prevent
				// visible blinking pixels.
				if (abs(abs(pixel_in_overflow->last_effect_frame_r) - abs(ditheringFrame)) > maxDitherFrames) {
					pixel_in_overflow->r = 0;
					pixel_in_overflow->last_effect_frame_r = ditheringFrame;
				}

				if (abs(abs(pixel_in_overflow->last_effect_frame_g) - abs(ditheringFrame)) > maxDitherFrames) {
					pixel_in_overflow->g = 0;
					pixel_in_overflow->last_effect_frame_g = ditheringFrame;
				}

				if (abs(abs(pixel_in_overflow->last_effect_frame_b) - abs(ditheringFrame)) > maxDitherFrames) {
					pixel_in_overflow->b = 0;
					pixel_in_overflow->last_effect_frame_b = ditheringFrame;
				}

				// Apply dithering overflow
				int32_t	ditheredR = interpolatedR;
				int32_t	ditheredG = interpolatedG;
				int32_t	ditheredB = interpolatedB;

				if (dithering_enabled) {
					ditheredR += pixel_in_overflow->r;
					ditheredG += pixel_in_overflow->g;
					ditheredB += pixel_in_overflow->b;
				}

				// Calculate and assign output values
				uint8_t r = pixel_out->r = min((ditheredR+0x80) >> 8, 255);
				uint8_t g = pixel_out->g = min((ditheredG+0x80) >> 8, 255);
				uint8_t b = pixel_out->b = min((ditheredB+0x80) >> 8, 255);

				// Check for interpolation effect
				if (r != (interpolatedR+0x80)>>8) pixel_in_overflow->last_effect_frame_r = ditheringFrame;
				if (g != (interpolatedG+0x80)>>8) pixel_in_overflow->last_effect_frame_g = ditheringFrame;
				if (b != (interpolatedB+0x80)>>8) pixel_in_overflow->last_effect_frame_b = ditheringFrame;

				// Recalculate Overflow
				// NOTE: For some strange reason, reading the values from pixel_out causes strange memory corruption. As such
				// we use temporary variables, r, g, and b. It probably has to do with things being loaded into the CPU cache
				// when read, as such, don't read pixel_out from here.
				if (dithering_enabled) {
					pixel_in_overflow->r = (int16_t)ditheredR - (r * 257);
					pixel_in_overflow->g = (int16_t)ditheredG - (g * 257);
					pixel_in_overflow->b = (int16_t)ditheredB - (b * 257);
				}
			}
		}

		// Render the frame
		ledscape_wait(g_frame_data.leds);
		ledscape_draw(g_frame_data.leds, buffer_index);

		pthread_mutex_unlock(&g_frame_data.mutex);

		// Output Timing Info
		gettimeofday(&stop_tv, NULL);
		timersub(&stop_tv, &start_tv, &delta_tv);

		frames++;
		delta_sum += delta_tv.tv_usec;
		if (stop_tv.tv_sec - last_report < report_interval)
			continue;
		last_report = stop_tv.tv_sec;

		delta_avg = delta_sum / frames;
		printf("[render] fps_info={frame_avg_usec: %6u, possible_fps: %.2f, actual_fps: %.2f, sample_frames: %u}\n",
			delta_avg,
			(1.0e6 / delta_avg),
			frames * 1.0 / report_interval,
			frames
		);

		frames = delta_sum = 0;
	}

	ledscape_close(g_frame_data.leds);
	pthread_exit(NULL);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Server Common

typedef struct
{
	uint8_t channel;
	uint8_t command;
	uint8_t len_hi;
	uint8_t len_lo;
} opc_cmd_t;

typedef enum
{
	OPC_SYSID_FADECANDY = 1,

	// Pending approval from the OPC folks
	OPC_SYSID_LEDSCAPE = 2
} opc_system_id_t;

typedef enum
{
	OPC_LEDSCAPE_CMD_GET_CONFIG = 1
} opc_ledscape_cmd_id_t;


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// UDP Server
//

void* udp_server_thread(void* unused_data)
{
	unused_data=unused_data; // Suppress Warnings
	fprintf(stderr, "Starting UDP server on port %d\n", g_server_config.udp_port);

	// Gen some fake data!
	uint32_t count = g_server_config.leds_per_strip*LEDSCAPE_NUM_STRIPS*3;
	uint8_t* data = malloc(count);

	uint8_t offset = 0;
	// for (;;) {
	// 	offset++;

	// 	for (uint32_t i=0; i<count; i+=3) {
	// 		// data[i] = (i + offset) % 64;
	// 		// data[i+1] = (i + offset + 256/3) % 64;
	// 		// data[i+2] = (i + offset + 512/3) % 64;
	// 		data[i] = data[i+1] = data[i+2] = (offset%2==0) ? 0 : 32;
	// 	}

	// 	set_next_frame_data(data, count);
	// 	usleep(1000000);
	// }

	pthread_exit(NULL);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// TCP Server
static int
tcp_socket(
	const int port
)
{
	const int sock = socket(AF_INET6, SOCK_STREAM, 0);
	struct sockaddr_in6 addr = {
		.sin6_family = AF_INET6,
		.sin6_port = htons(port),
		.sin6_addr = in6addr_any,
	};

	if (sock < 0)
		return -1;
	if (bind(sock, (const struct sockaddr*) &addr, sizeof(addr)) < 0)
		return -1;
	if (listen(sock, 5) == -1)
		return -1;

	return sock;
}

void* tcp_server_thread(void* unused_data)
{
	unused_data=unused_data; // Suppress Warnings


	pthread_mutex_lock(&g_server_config.mutex);
	fprintf(stderr, "[tcp] Starting TCP server on port %d\n", g_server_config.tcp_port);

	const int sock = tcp_socket(g_server_config.tcp_port);
	pthread_mutex_unlock(&g_server_config.mutex);

	if (sock < 0)
		die("[tcp] socket port %d failed: %s\n", g_server_config.tcp_port, strerror(errno));

	uint8_t buf[65536];

	int fd;
	while ((fd = accept(sock, NULL, NULL)) >= 0)
	{
		printf("[tcp] Client connected!\n");

		while(1)
		{
			opc_cmd_t cmd;
			ssize_t rlen = read(fd, &cmd, sizeof(cmd));
			if (rlen < 0)
				die("[tcp] recv failed: %s\n", strerror(errno));
			if (rlen == 0)
			{
				close(fd);
				break;
			}

			const size_t cmd_len = cmd.len_hi << 8 | cmd.len_lo;

			//warn("cmd=%d; size=%zu\n", cmd.command, cmd_len);

			size_t offset = 0;
			while (offset < cmd_len)
			{
				rlen = read(fd, buf + offset, cmd_len - offset);
				if (rlen < 0)
					die("[tcp] recv failed: %s\n", strerror(errno));
				if (rlen == 0)
					break;
				offset += rlen;
			}

			if (cmd.command == 0) {
				set_next_frame_data(buf, cmd_len);
			} else if (cmd.command == 255) {
				// System specific commands
				const uint16_t system_id = buf[0] << 8 | buf[1];

				if (system_id == OPC_SYSID_LEDSCAPE) {
					const opc_ledscape_cmd_id_t ledscape_cmd_id = buf[2];

					 if (ledscape_cmd_id == OPC_LEDSCAPE_CMD_GET_CONFIG) {

						warn("[tcp] Responding to config request\n");
						write(fd, g_server_config.json, strlen(g_server_config.json)+1);
					} else {
						warn("[tcp] WARN: Received command for unsupported LEDscape Command: %d\n", (int)ledscape_cmd_id);
					}
				} else {
					warn("[tcp] WARN: Received command for unsupported system-id: %d\n", (int)system_id);
				}
			}
		}
	}

	pthread_exit(NULL);
}
