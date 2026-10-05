/* Development-only probe endpoint (built with -DEXPORTER_PROBE).
 *
 * Calls a few undocumented libkernel functions with generous zeroed buffers
 * and dumps whatever comes back, so their shape can be worked out on real
 * hardware before they become metrics. Never part of a release build. */
#ifdef EXPORTER_PROBE

#include <sys/types.h>
#include <sys/param.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

#include "probe.h"

int sceKernelGetCurrentFanDuty(void* out);
int sceKernelIccGetThermalAlert(void* out);
int sceKernelGetCpuUsage(void* buf, int* count);
int sceKernelGetCpuUsageAll(void* buf, int* count);
int sceKernelGetCpuUsageProc(int pid, void* buf, int* count);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t* out_start, size_t* out_size);
int sceKernelAvailableFlexibleMemorySize(size_t* out);
int sceKernelConfiguredFlexibleMemorySize(size_t* out);

#define APPEND(...) do { int n_ = snprintf(out + len, cap > len ? cap - len : 0, __VA_ARGS__); if (n_ > 0) len += (size_t)n_; } while (0)

static size_t hexdump(char* out, size_t cap, const void* data, size_t n)
{
	size_t len = 0;
	const unsigned char* p = (const unsigned char*)data;
	for (size_t i = 0; i < n; i++) {
		APPEND("%02x%s", p[i], (i % 16 == 15) ? "\n" : " ");
	}
	if (n % 16) APPEND("\n");
	return len;
}

static size_t ints(char* out, size_t cap, const void* data, size_t n_ints)
{
	size_t len = 0;
	const int* p = (const int*)data;
	for (size_t i = 0; i < n_ints; i++) APPEND("%d ", p[i]);
	APPEND("\n");
	return len;
}

size_t probe_run(const char* what, char* out, size_t cap)
{
	size_t len = 0;
	static unsigned char buf[4096];
	int rc, n;

	if (strcmp(what, "fan") == 0) {
		memset(buf, 0, sizeof(buf));
		rc = sceKernelGetCurrentFanDuty(buf);
		APPEND("sceKernelGetCurrentFanDuty(buf) rc=%d (0x%x)\nints: ", rc, (unsigned)rc);
		len += ints(out + len, cap - len, buf, 8);
		len += hexdump(out + len, cap - len, buf, 32);
		memset(buf, 0, sizeof(buf));
		rc = sceKernelIccGetThermalAlert(buf);
		APPEND("sceKernelIccGetThermalAlert(buf) rc=%d (0x%x)\n", rc, (unsigned)rc);
		len += hexdump(out + len, cap - len, buf, 32);
		return len;
	}
	if (strcmp(what, "cpu") == 0) {
		memset(buf, 0, sizeof(buf));
		n = 64;
		rc = sceKernelGetCpuUsage(buf, &n);
		APPEND("sceKernelGetCpuUsage(buf, &n=64) rc=%d (0x%x) n=%d\n", rc, (unsigned)rc, n);
		len += hexdump(out + len, cap - len, buf, 256);
		return len;
	}
	if (strcmp(what, "cpuall") == 0) {
		memset(buf, 0, sizeof(buf));
		n = 64;
		rc = sceKernelGetCpuUsageAll(buf, &n);
		APPEND("sceKernelGetCpuUsageAll(buf, &n=64) rc=%d (0x%x) n=%d\n", rc, (unsigned)rc, n);
		len += hexdump(out + len, cap - len, buf, 256);
		return len;
	}
	if (strcmp(what, "cpuproc") == 0) {
		memset(buf, 0, sizeof(buf));
		n = 64;
		rc = sceKernelGetCpuUsageProc(getpid(), buf, &n);
		APPEND("sceKernelGetCpuUsageProc(pid=%d, buf, &n=64) rc=%d (0x%x) n=%d\n", getpid(), rc, (unsigned)rc, n);
		len += hexdump(out + len, cap - len, buf, 256);
		return len;
	}
	if (strcmp(what, "mem") == 0) {
		size_t total = sceKernelGetDirectMemorySize();
		int64_t start = 0;
		size_t size = 0;
		rc = sceKernelAvailableDirectMemorySize(0, (int64_t)total, 0, &start, &size);
		APPEND("direct memory size=%zu\navailable: rc=%d (0x%x) start=%lld size=%zu\n", total, rc, (unsigned)rc, (long long)start, size);
		size_t flex = 0;
		rc = sceKernelAvailableFlexibleMemorySize(&flex);
		APPEND("flexible available: rc=%d size=%zu\n", rc, flex);
		flex = 0;
		rc = sceKernelConfiguredFlexibleMemorySize(&flex);
		APPEND("flexible configured: rc=%d size=%zu\n", rc, flex);
		return len;
	}
	if (strcmp(what, "procs") == 0) {
		int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC };
		size_t sz = 0;
		if (sysctl(mib, 3, NULL, &sz, NULL, 0) != 0) { APPEND("sysctl size failed\n"); return len; }
		static unsigned char big[256 * 1024];
		if (sz > sizeof(big)) sz = sizeof(big);
		if (sysctl(mib, 3, big, &sz, NULL, 0) != 0) { APPEND("sysctl read failed\n"); return len; }
		size_t count = sz / sizeof(struct kinfo_proc);
		APPEND("kinfo_proc size=%zu bytes, %zu processes (structsize field of first: %d)\n", sizeof(struct kinfo_proc), count, ((struct kinfo_proc*)big)->ki_structsize);
		APPEND("%6s %-20s %12s %10s %8s\n", "pid", "comm", "runtime_us", "rss_pages", "pctcpu");
		for (size_t i = 0; i < count && i < 120; i++) {
			const struct kinfo_proc* k = (const struct kinfo_proc*)(big + i * sizeof(struct kinfo_proc));
			char comm[COMMLEN + 2];
			memcpy(comm, k->ki_comm, COMMLEN + 1);
			comm[COMMLEN + 1] = 0;
			APPEND("%6d %-20.20s %12llu %10ld %8u\n", (int)k->ki_pid, comm, (unsigned long long)k->ki_runtime, (long)k->ki_rssize, (unsigned)k->ki_pctcpu);
		}
		return len;
	}
	if (strcmp(what, "sensors") == 0) {
		int t;
		for (int s = 0; s < 16; s++) {
			extern int sceKernelGetSocSensorTemperature(int, int*);
			t = -1;
			rc = sceKernelGetSocSensorTemperature(s, &t);
			APPEND("soc sensor %2d: rc=%d (0x%x) temp=%d\n", s, rc, (unsigned)rc, t);
		}
		return len;
	}
	APPEND("probes: fan cpu cpuall cpuproc mem procs sensors\n");
	return len;
}

#endif
