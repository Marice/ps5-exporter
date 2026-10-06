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
#include <stddef.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <unistd.h>

#include "probe.h"

#include "buf.h"

int sceKernelGetCurrentFanDuty(uint16_t* duty, uint64_t* chassis);
int sceKernelIccGetThermalAlert(void* out);
int sceKernelGetCpuUsage(void* buf, int* count);
int sceKernelGetCpuUsageAll(int* per_core_pct, int* count_out);
int sceKernelGetCpuUsageProc(int pid, void* buf, int* count);
int sceKernelGetSocPowerConsumption(uint64_t* out, double reserved);
int sceKernelIccGetPowerNumberOfBootShutdown(uint64_t* out);
int sceKernelIccGetPowerOperatingTime(uint64_t* out);
int sceKernelGetBasicProductShape(int* out);
int sceKernelGetCpumode(void);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t* out_start, size_t* out_size);
int sceKernelAvailableFlexibleMemorySize(size_t* out);
int sceKernelConfiguredFlexibleMemorySize(size_t* out);

#define APPEND(...) buf_addf(b, __VA_ARGS__)

static void hexdump(Buf* b, const void* data, size_t n)
{
	const unsigned char* p = (const unsigned char*)data;
	for (size_t i = 0; i < n; i++) {
		APPEND("%02x%s", p[i], (i % 16 == 15) ? "\n" : " ");
	}
	if (n % 16) APPEND("\n");
}

static void ints(Buf* b, const void* data, size_t n_ints)
{
	const int* p = (const int*)data;
	for (size_t i = 0; i < n_ints; i++) APPEND("%d ", p[i]);
	APPEND("\n");
}

void probe_run(const char* what, Buf* b)
{
	static unsigned char buf[4096];
	int rc, n;

	if (strcmp(what, "fan") == 0) {
		uint16_t duty = 0xffff;
		uint64_t chassis = 0;
		rc = sceKernelGetCurrentFanDuty(&duty, &chassis);
		APPEND("sceKernelGetCurrentFanDuty(&duty,&chassis) rc=%d (0x%x) duty=%u (%.1f%% of 1024) chassis=%llu\n",
		       rc, (unsigned)rc, (unsigned)duty, duty * 100.0 / 1024.0, (unsigned long long)chassis);
		memset(buf, 0, sizeof(buf));
		rc = sceKernelIccGetThermalAlert(buf);
		APPEND("sceKernelIccGetThermalAlert(buf) rc=%d (0x%x)\n", rc, (unsigned)rc);
		hexdump(b, buf, 32);
		return;
	}
	if (strcmp(what, "cpu") == 0) {
		memset(buf, 0, sizeof(buf));
		n = 64;
		rc = sceKernelGetCpuUsage(buf, &n);
		APPEND("sceKernelGetCpuUsage(buf, &n=64) rc=%d (0x%x) n=%d\n", rc, (unsigned)rc, n);
		hexdump(b, buf, 256);
		return;
	}
	if (strcmp(what, "cpuall") == 0) {
		int per_core[16];
		for (int i = 0; i < 16; i++) per_core[i] = -1;
		n = 0;
		rc = sceKernelGetCpuUsageAll(per_core, &n);
		APPEND("sceKernelGetCpuUsageAll(per_core,&count) rc=%d (0x%x) count=%d\nvalues: ", rc, (unsigned)rc, n);
		ints(b, per_core, 16);
		return;
	}
	if (strcmp(what, "power") == 0) {
		uint64_t pw[16];
		memset(pw, 0, sizeof(pw));
		rc = sceKernelGetSocPowerConsumption(pw, 0.0);
		APPEND("sceKernelGetSocPowerConsumption(buf,0.0) rc=%d (0x%x)\n", rc, (unsigned)rc);
		for (int i = 0; i < 8; i++) APPEND("  [%d] %llu (0x%llx)\n", i, (unsigned long long)pw[i], (unsigned long long)pw[i]);
		uint64_t v = 0;
		rc = sceKernelIccGetPowerOperatingTime(&v);
		APPEND("IccGetPowerOperatingTime rc=%d value=%llu (%.1f hours)\n", rc, (unsigned long long)v, v / 3600.0);
		v = 0;
		rc = sceKernelIccGetPowerNumberOfBootShutdown(&v);
		APPEND("IccGetPowerNumberOfBootShutdown rc=%d value=%llu\n", rc, (unsigned long long)v);
		int shape = -1;
		rc = sceKernelGetBasicProductShape(&shape);
		APPEND("GetBasicProductShape rc=%d shape=%d\n", rc, shape);
		APPEND("GetCpumode = %d\n", sceKernelGetCpumode());
		return;
	}
	if (strcmp(what, "cpuproc") == 0) {
		memset(buf, 0, sizeof(buf));
		n = 64;
		rc = sceKernelGetCpuUsageProc(getpid(), buf, &n);
		APPEND("sceKernelGetCpuUsageProc(pid=%d, buf, &n=64) rc=%d (0x%x) n=%d\n", getpid(), rc, (unsigned)rc, n);
		hexdump(b, buf, 256);
		return;
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
		return;
	}
	if (strcmp(what, "procs") == 0) {
		int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC };
		size_t sz = 0;
		if (sysctl(mib, 3, NULL, &sz, NULL, 0) != 0) { APPEND("sysctl size failed\n"); return; }
		static unsigned char big[256 * 1024];
		if (sz > sizeof(big)) sz = sizeof(big);
		if (sysctl(mib, 3, big, &sz, NULL, 0) != 0) { APPEND("sysctl read failed\n"); return; }
		int structsize = ((struct kinfo_proc*)big)->ki_structsize;
		APPEND("sysctl returned %zu bytes\n", sz);
		APPEND("SDK sizeof(kinfo_proc) = %zu, offsetof(ki_comm) = %zu, COMMLEN = %d\n", sizeof(struct kinfo_proc), offsetof(struct kinfo_proc, ki_comm), COMMLEN);
		APPEND("kernel ki_structsize = %d (0x%x)\n", structsize, (unsigned)structsize);
		if (structsize > 0 && (size_t)structsize <= sz) {
			APPEND("=> %zu records at the kernel stride\n", sz / (size_t)structsize);
			/* Show where the name sits with the kernel's own stride. */
			for (int r = 0; r < 3 && (size_t)(r + 1) * structsize <= sz; r++) {
				const unsigned char* rec = big + (size_t)r * structsize;
				APPEND("record %d: pid field=%d name@%zu='%.20s'\n", r,
				       (int)((const struct kinfo_proc*)rec)->ki_pid,
				       offsetof(struct kinfo_proc, ki_comm),
				       (const char*)(rec + offsetof(struct kinfo_proc, ki_comm)));
			}
		}
		size_t count = sz / sizeof(struct kinfo_proc);
		APPEND("%6s %-20s %12s %10s %8s\n", "pid", "comm", "runtime_us", "rss_pages", "pctcpu");
		for (size_t i = 0; i < count && i < 120; i++) {
			const struct kinfo_proc* k = (const struct kinfo_proc*)(big + i * sizeof(struct kinfo_proc));
			char comm[COMMLEN + 2];
			memcpy(comm, k->ki_comm, COMMLEN + 1);
			comm[COMMLEN + 1] = 0;
			APPEND("%6d %-20.20s %12llu %10ld %8u\n", (int)k->ki_pid, comm, (unsigned long long)k->ki_runtime, (long)k->ki_rssize, (unsigned)k->ki_pctcpu);
		}
		return;
	}
	if (strcmp(what, "sensors") == 0) {
		int t;
		for (int s = 0; s < 16; s++) {
			extern int sceKernelGetSocSensorTemperature(int, int*);
			t = -1;
			rc = sceKernelGetSocSensorTemperature(s, &t);
			APPEND("soc sensor %2d: rc=%d (0x%x) temp=%d\n", s, rc, (unsigned)rc, t);
		}
		return;
	}
	APPEND("probes: fan cpu cpuall cpuproc power mem procs sensors\n");
}

#endif
