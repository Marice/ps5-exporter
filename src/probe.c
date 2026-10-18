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
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "probe.h"

#include "buf.h"

int sceKernelGetCurrentFanDuty(uint16_t* duty, uint64_t* chassis);
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
/* Getters ps5-hwinfo (drakmor, GPL-3.0) calls on consoles; signatures from there. */
int sceKernelGetSocClock(uint32_t* out_raw_domains);
int sceKernelGetCpuCoreClock(int* out_mhz_per_core);
int sceKernelIccGetUSBPowerState(uint8_t* out_state);
int sceKernelIccGetBDPowerState(uint8_t* out_state);
int sceKernelGetUniversalMode(int* out_mode);
int sceKernelGetAppInfo(int pid, void* out);
int get_page_table_stats(int vm, int type, int* total, int* free);

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
	if (strcmp(what, "rails") == 0) {
		/* 0x70 bytes: 8 rails of {mW, mV, mA}, then aux words (ps5-hwinfo's layout). */
		uint32_t w[32];
		memset(w, 0, sizeof(w));
		rc = sceKernelGetSocPowerConsumption((uint64_t*)w, 0.0);
		APPEND("sceKernelGetSocPowerConsumption rc=%d (0x%x)\n", rc, (unsigned)rc);
		unsigned long long sum = 0;
		for (int r = 0; r < 8; r++) {
			APPEND("rail %d: %u mW  %u mV  %u mA\n", r, w[r * 3], w[r * 3 + 1], w[r * 3 + 2]);
			sum += w[r * 3];
		}
		APPEND("sum %.3f W\naux:", sum / 1000.0);
		for (int i = 24; i < 32; i++) APPEND(" %u", w[i]);
		APPEND("\n");
		hexdump(b, w, 0x80);
		return;
	}
	if (strcmp(what, "clocks") == 0) {
		uint32_t soc[64];
		memset(soc, 0, sizeof(soc));
		rc = sceKernelGetSocClock(soc);
		APPEND("sceKernelGetSocClock rc=%d (0x%x)\n", rc, (unsigned)rc);
		for (int i = 0; i < 32; i++) APPEND("soc[%d] %u\n", i, soc[i]);
		int core[32];
		for (int i = 0; i < 32; i++) core[i] = -1;
		rc = sceKernelGetCpuCoreClock(core);
		APPEND("sceKernelGetCpuCoreClock rc=%d (0x%x)\ncores:", rc, (unsigned)rc);
		ints(b, core, 16);
		int mode = -1;
		rc = sceKernelGetUniversalMode(&mode);
		APPEND("sceKernelGetUniversalMode rc=%d mode=%d\n", rc, mode);
		return;
	}
	if (strcmp(what, "ram") == 0) {
		static const char* names[] = { "hw.pagesize", "vm.stats.vm.v_page_count", "vm.stats.vm.v_free_count",
		                               "vm.stats.vm.v_inactive_count", "vm.stats.vm.v_cache_count",
		                               "vm.stats.vm.v_active_count", "vm.stats.vm.v_wire_count", "hw.usermem",
		                               "vm.swap_total", "vm.swap_avail", "vm.stats.vm0.v_wire_count",
		                               "vm.stats.vm10.v_wire_count" };
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
			uint64_t v = 0;
			size_t len = sizeof(v);
			rc = sysctlbyname(names[i], &v, &len, NULL, 0);
			APPEND("%-28s rc=%d len=%zu value=%llu\n", names[i], rc, len,
			       (unsigned long long)(len == 4 ? (uint32_t)v : v));
		}
		for (int type = 1; type <= 2; type++) {
			int total = -1, free_mb = -1;
			rc = get_page_table_stats(0, type, &total, &free_mb);
			APPEND("get_page_table_stats(0,%d) rc=%d total=%d free=%d\n", type, rc, total, free_mb);
		}
		return;
	}
	if (strcmp(what, "icc") == 0) {
		uint8_t st = 0xff;
		rc = sceKernelIccGetUSBPowerState(&st);
		APPEND("IccGetUSBPowerState rc=%d state=%u\n", rc, st);
		st = 0xff;
		rc = sceKernelIccGetBDPowerState(&st);
		APPEND("IccGetBDPowerState rc=%d state=%u\n", rc, st);
		/* sceKernelIccGetThermalAlert is not exported on 13.60: the payload did not start with it. */
		return;
	}
	if (strcmp(what, "apps") == 0) {
		/* Every process with app info: the running title should be among them (kind >= 0x1000000 per ps5-hwinfo). */
		int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC };
		size_t sz = 0;
		static unsigned char big[256 * 1024];
		if (sysctl(mib, 3, NULL, &sz, NULL, 0) != 0) { APPEND("sysctl size failed\n"); return; }
		if (sz > sizeof(big)) sz = sizeof(big);
		if (sysctl(mib, 3, big, &sz, NULL, 0) != 0) { APPEND("sysctl read failed\n"); return; }
		int stride = ((struct kinfo_proc*)big)->ki_structsize;
		if (stride <= 0) { APPEND("no record size\n"); return; }
		for (size_t off = 0; off + (size_t)stride <= sz; off += (size_t)stride) {
			const struct kinfo_proc* k = (const struct kinfo_proc*)(big + off);
			unsigned char info[128];
			memset(info, 0, sizeof(info));
			if (sceKernelGetAppInfo(k->ki_pid, info) != 0) continue;
			uint32_t kind = 0;
			memcpy(&kind, info + 80, sizeof(kind));
			APPEND("pid %d '%.20s' kind 0x%x\n", (int)k->ki_pid, k->ki_comm, kind);
			hexdump(b, info, 96);
		}
		return;
	}
	if (strcmp(what, "fps") == 0) {
		/* The display's flip counter, as ps5-hud-dualsense-overlay (GPL-3.0) reads it after Common FPS for PS5:
		 * ioctl 0x80308217 on /dev/dce with selector 0x10000000A and mask 0x8000000000; the count is at offset 8
		 * of the block the kernel writes (128 bytes on 13.60). The block goes to a 16 KB mapping with a guard page
		 * behind it and a canary, so a longer write is seen instead of corrupting memory. */
		enum { BYTES = 0x4000 };
		unsigned char* map = mmap(NULL, BYTES * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
		if (map == MAP_FAILED) { APPEND("mmap failed\n"); return; }
		if (mprotect(map + BYTES, BYTES, PROT_NONE) != 0) { APPEND("mprotect failed\n"); munmap(map, BYTES * 2); return; }
		struct { uint64_t selector, mask, output, reserved[3]; } __attribute__((aligned(64))) arg;
		int fd = open("/dev/dce", O_RDWR);
		APPEND("open /dev/dce fd=%d errno=%d\n", fd, fd < 0 ? errno : 0);
		if (fd >= 0) {
			unsigned long request = 0x80308217UL;
			uint64_t counts[2] = { 0, 0 };
			struct timespec at[2];
			for (int i = 0; i < 2; i++) {
				memset(map, 0xA5, BYTES);
				memset(&arg, 0, sizeof(arg));
				arg.selector = 0x10000000AULL;
				arg.mask = 0x8000000000ULL;
				arg.output = (uint64_t)(uintptr_t)map;
				rc = ioctl(fd, request, &arg);
				if (rc < 0 && errno == EINVAL && request == 0x80308217UL) {
					request = (unsigned long)0xFFFFFFFF80308217ULL;
					memset(map, 0xA5, BYTES);
					rc = ioctl(fd, request, &arg);
				}
				clock_gettime(CLOCK_MONOTONIC, &at[i]);
				size_t written = BYTES;
				while (written > 0 && map[written - 1] == 0xA5) written--;
				APPEND("ioctl %#lx rc=%d errno=%d written=%zu\n", request, rc, rc < 0 ? errno : 0, written);
				if (rc < 0) break;
				if (i == 0) hexdump(b, map, written > 128 ? 128 : written);
				memcpy(&counts[i], map + 8, sizeof(counts[i]));
				APPEND("count %llu\n", (unsigned long long)counts[i]);
				if (i == 0) usleep(1000 * 1000);
			}
			double dt = (at[1].tv_sec - at[0].tv_sec) + (at[1].tv_nsec - at[0].tv_nsec) / 1e9;
			if (counts[1] >= counts[0] && dt > 0) APPEND("fps %.1f over %.2f s\n", (counts[1] - counts[0]) / dt, dt);
			close(fd);
		}
		munmap(map, BYTES * 2);
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
	APPEND("probes: fan cpu cpuall cpuproc power rails clocks ram icc apps fps mem procs sensors\n");
}

#endif
