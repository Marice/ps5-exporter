/* Frames per second of the running game.
 *
 * The display controller counts the frames it flips. Read with ioctl
 * 0x80308217 on /dev/dce, selector 0x10000000A and mask 0x8000000000, the
 * count is at offset 8 of the block the kernel writes: 128 bytes on 13.60,
 * more than the 0x60 some tools reserve. Request, selector and offset come
 * from Common FPS for PS5 (porhe911), as erickdavestech/ps5-hud-dualsense-
 * overlay documents them (GPL-3.0); etaHEN and onionHEN read it the same way.
 *
 * The block goes to a 16 KB mapping with a guard page behind it, so a longer
 * write faults instead of corrupting memory. Without a game the ioctl fails
 * with ENOENT; the thread then waits 5 seconds before it tries again. One
 * thread samples once a second, so a scrape never waits for a second sample.
 * Checked on a 13.60 Pro: 60 FPS in a 60 FPS game. */
#include "fps.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "metrics.h"

#define DCE_REQUEST 0x80308217UL
#define DCE_REQUEST_SIGNED ((unsigned long)0xFFFFFFFF80308217ULL)
#define DCE_SELECTOR 0x10000000AULL
#define DCE_MASK 0x8000000000ULL
#define DCE_COUNT_OFFSET 8
#define DCE_BYTES 0x4000
#define DCE_CANARY 0xA5
/* A frame rate above this is a counter jump (seen once while a game loaded), not frames. */
#define FPS_MAX 240.0

struct dce_arg {
	uint64_t selector;
	uint64_t mask;
	uint64_t output;
	uint64_t reserved[3];
} __attribute__((aligned(64)));

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static double g_fps = -1.0;
static double g_fps_at = 0.0;

static double now_seconds(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

/* One read of the flip counter; -1 when it fails. */
static int read_count(int fd, unsigned char* block, unsigned long* request, uint64_t* count)
{
	static struct dce_arg arg;
	memset(block, DCE_CANARY, DCE_BYTES);
	memset(&arg, 0, sizeof(arg));
	arg.selector = DCE_SELECTOR;
	arg.mask = DCE_MASK;
	arg.output = (uint64_t)(uintptr_t)block;
	int rc = ioctl(fd, *request, &arg);
	if (rc < 0 && errno == EINVAL && *request == DCE_REQUEST) {
		*request = DCE_REQUEST_SIGNED;
		memset(block, DCE_CANARY, DCE_BYTES);
		rc = ioctl(fd, *request, &arg);
	}
	if (rc < 0) return -1;
	size_t written = DCE_BYTES;
	while (written > 0 && block[written - 1] == DCE_CANARY) written--;
	if (written < DCE_COUNT_OFFSET + sizeof(*count)) return -1;
	memcpy(count, block + DCE_COUNT_OFFSET, sizeof(*count));
	return 0;
}

static void* sample(void* unused)
{
	(void)unused;
	unsigned char* map = mmap(NULL, DCE_BYTES * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (map == MAP_FAILED) return NULL;
	if (mprotect(map + DCE_BYTES, DCE_BYTES, PROT_NONE) != 0) {
		munmap(map, DCE_BYTES * 2);
		return NULL;
	}
	unsigned long request = DCE_REQUEST;
	int fd = -1;
	uint64_t last = 0;
	double last_at = 0.0;
	for (;;) {
		if (fd < 0) fd = open("/dev/dce", O_RDWR);
		uint64_t count = 0;
		if (fd < 0 || read_count(fd, map, &request, &count) != 0) {
			/* No game (ENOENT) or no device: start over later. */
			if (fd >= 0) close(fd);
			fd = -1;
			last_at = 0.0;
			sleep(5);
			continue;
		}
		double at = now_seconds();
		if (last_at > 0.0 && count >= last && at - last_at > 0.5) {
			double fps = (count - last) / (at - last_at);
			if (fps <= FPS_MAX) {
				pthread_mutex_lock(&g_lock);
				g_fps = fps;
				g_fps_at = at;
				pthread_mutex_unlock(&g_lock);
			}
		}
		last = count;
		last_at = at;
		sleep(1);
	}
	return NULL;
}

void fps_start(void)
{
	pthread_t thread;
	if (pthread_create(&thread, NULL, sample, NULL) == 0) pthread_detach(thread);
}

void metrics_fps(Buf* b)
{
	pthread_mutex_lock(&g_lock);
	double fps = g_fps, at = g_fps_at;
	pthread_mutex_unlock(&g_lock);
	/* A sample older than 3 seconds means the game stopped. */
	if (fps < 0.0 || now_seconds() - at > 3.0) {
		metrics_note_collector("fps", 0);
		return;
	}
	buf_addf(b, "# HELP ps5_fps Frames per second the display shows, from its flip counter (while a game runs).\n# TYPE ps5_fps gauge\nps5_fps %.1f\n", fps);
	metrics_note_collector("fps", 1);
}
