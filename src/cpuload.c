/* CPU load per logical CPU.
 *
 * sceKernelGetCpuUsage(entries, &count) lists every thread's run time
 * ({pid, tid, user timeval, system timeval}, 40 bytes each). Each logical CPU
 * has an idle thread, SceIdleCpuN (SceIdleCpuRv for lane 13), found by name
 * with sceKernelGetThreadName. Load = 1 - idle time / wall time between two
 * samples. Method from drakmor/ps5-hwinfo and erickdavestech/ps5-hud-
 * dualsense-overlay (both GPL-3.0); sceKernelGetCpuUsageAll, which v0.2.0
 * used, answers 0x80020016 on 13.60.
 *
 * The kernel reported 1438 threads to a caller that offered room for 64, so
 * the list gets room for 3072 (as ps5-hwinfo gives it) in a mapping with a
 * guard page behind it: a longer write faults instead of corrupting memory. */
#include "cpuload.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "metrics.h"
#include "optional.h"


#define LANES 16
#define MAX_THREADS 3072

struct thread_time {
	uint32_t pid;
	uint32_t tid;
	int64_t user_sec, user_usec;
	int64_t system_sec, system_usec;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static double g_load[LANES];
static int g_have[LANES];
static double g_at;

static double now_seconds(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static double run_seconds(const struct thread_time* t)
{
	return (double)(t->user_sec + t->system_sec) + (double)(t->user_usec + t->system_usec) / 1e6;
}

/* The lane an idle thread serves, or -1. */
static int idle_lane(uint32_t tid)
{
	char name[0x40];
	memset(name, 0, sizeof(name));
	int (*get_name)(uint32_t, char*) = OPTIONAL(int (*)(uint32_t, char*), sceKernelGetThreadName);
	if (!get_name || get_name(tid, name) != 0) return -1;
	if (strcmp(name, "SceIdleCpuRv") == 0) return 13;
	int lane = -1;
	if (sscanf(name, "SceIdleCpu%d", &lane) == 1 && lane >= 0 && lane < LANES && lane != 13) return lane;
	return -1;
}

static void* sample(void* unused)
{
	(void)unused;
	size_t bytes = (sizeof(struct thread_time) * MAX_THREADS + 16383) & ~(size_t)16383;
	unsigned char* map = mmap(NULL, bytes + 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
	if (map == MAP_FAILED) return NULL;
	if (mprotect(map + bytes, 16384, PROT_NONE) != 0) {
		munmap(map, bytes + 16384);
		return NULL;
	}
	struct thread_time* list = (struct thread_time*)map;
	uint32_t idle_tid[LANES];
	double last_idle[LANES];
	memset(idle_tid, 0, sizeof(idle_tid));
	double last_at = 0.0;
	for (;; sleep(2)) {
		int32_t count = MAX_THREADS;
		int (*get_usage)(void*, int32_t*) = OPTIONAL(int (*)(void*, int32_t*), sceKernelGetCpuUsage);
		if (!get_usage) return NULL; /* not on this firmware */
		if (get_usage(list, &count) != 0 || count <= 0 || count > MAX_THREADS) {
			last_at = 0.0;
			continue;
		}
		double at = now_seconds();
		double idle[LANES];
		int found[LANES];
		memset(found, 0, sizeof(found));
		for (int32_t i = 0; i < count; i++) {
			int lane = -1;
			for (int l = 0; l < LANES; l++) {
				if (idle_tid[l] != 0 && idle_tid[l] == list[i].tid) {
					lane = l;
					break;
				}
			}
			if (lane < 0) {
				/* Names are looked up only until every lane has its idle thread. */
				int all = 1;
				for (int l = 0; l < LANES; l++) all &= idle_tid[l] != 0;
				if (all) continue;
				lane = idle_lane(list[i].tid);
				if (lane < 0) continue;
				idle_tid[lane] = list[i].tid;
			}
			idle[lane] = run_seconds(&list[i]);
			found[lane] = 1;
		}
		if (last_at > 0.0) {
			double wall = at - last_at;
			pthread_mutex_lock(&g_lock);
			for (int l = 0; l < LANES; l++) {
				g_have[l] = found[l] && wall > 0.5;
				if (!g_have[l]) continue;
				double load = 1.0 - (idle[l] - last_idle[l]) / wall;
				g_load[l] = load < 0.0 ? 0.0 : load > 1.0 ? 1.0 : load;
			}
			g_at = at;
			pthread_mutex_unlock(&g_lock);
		}
		for (int l = 0; l < LANES; l++) {
			if (found[l]) last_idle[l] = idle[l];
			else idle_tid[l] = 0; /* gone: look it up again */
		}
		last_at = at;
	}
	return NULL;
}

void cpuload_start(void)
{
	pthread_t thread;
	if (pthread_create(&thread, NULL, sample, NULL) == 0) pthread_detach(thread);
}

void metrics_cpuload(Buf* b)
{
	double load[LANES];
	int have[LANES];
	pthread_mutex_lock(&g_lock);
	memcpy(load, g_load, sizeof(load));
	memcpy(have, g_have, sizeof(have));
	double at = g_at;
	pthread_mutex_unlock(&g_lock);
	if (at == 0.0 || now_seconds() - at > 10.0) {
		metrics_note_collector("cpu_usage", 0);
		return;
	}
	int lanes = 0;
	size_t mark = buf_mark(b);
	buf_addf(b, "# HELP ps5_cpu_usage_ratio CPU usage per logical CPU (16), 0 to 1, from the idle threads. Use avg() for the overall load.\n# TYPE ps5_cpu_usage_ratio gauge\n");
	for (int l = 0; l < LANES; l++) {
		if (!have[l]) continue;
		buf_addf(b, "ps5_cpu_usage_ratio{core=\"%d\"} %.3f\n", l, load[l]);
		lanes++;
	}
	if (lanes == 0) {
		buf_rewind(b, mark);
		metrics_note_collector("cpu_usage", 0);
		return;
	}
	buf_addf(b, "# HELP ps5_cpu_cores Logical CPUs with a usage figure.\n# TYPE ps5_cpu_cores gauge\nps5_cpu_cores %d\n", lanes);
	metrics_note_collector("cpu_usage", 1);
}
