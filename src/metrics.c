/* System metrics read through the console's libc and libkernel: no kernel
   offsets, so these keep working across firmware versions. */
#include "metrics.h"

#include <sys/types.h>
#include <sys/param.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <time.h>

#include <ps5/kernel.h>

/* libkernel exports used by the SDK's hwinfo sample. */
int sceKernelGetCpuTemperature(int* celsius);
int sceKernelGetSocSensorTemperature(int sensor, int* celsius);
long sceKernelGetCpuFrequency(void);
int sceKernelGetHwModelName(char* out);
int sceKernelGetSystemSwVersion(void* out);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t* out_start, size_t* out_size);

#define APPEND(...) do { int n_ = snprintf(out + len, cap > len ? cap - len : 0, __VA_ARGS__); if (n_ > 0) len += (size_t)n_; } while (0)

static void escape_label(const char* in, char* out, size_t cap)
{
	size_t j = 0;
	for (size_t i = 0; in[i] && j + 2 < cap; i++) {
		char c = in[i];
		if (c == '"' || c == '\\') out[j++] = '\\';
		if (c == '\n') { out[j++] = ' '; continue; }
		out[j++] = c;
	}
	out[j] = 0;
}

static size_t temperatures(char* out, size_t cap)
{
	size_t len = 0;
	int t = 0;
	APPEND("# HELP ps5_temperature_celsius Temperature reported by the console.\n# TYPE ps5_temperature_celsius gauge\n");
	if (sceKernelGetCpuTemperature(&t) == 0) APPEND("ps5_temperature_celsius{sensor=\"cpu\"} %d\n", t);
	/* Report every SoC sensor index the kernel answers for (0..15); the
	   console has more than the four the SDK sample shows. */
	for (int s = 0; s < 16; s++) {
		t = -1000;
		if (sceKernelGetSocSensorTemperature(s, &t) == 0 && t > -100 && t < 200) APPEND("ps5_temperature_celsius{sensor=\"soc%d\"} %d\n", s, t);
	}
	long hz = sceKernelGetCpuFrequency();
	if (hz > 0) APPEND("# HELP ps5_cpu_frequency_hertz CPU frequency.\n# TYPE ps5_cpu_frequency_hertz gauge\nps5_cpu_frequency_hertz %ld\n", hz);
	return len;
}

static size_t uptime(char* out, size_t cap)
{
	size_t len = 0;
	struct timeval boot;
	size_t sz = sizeof(boot);
	int mib[2] = { CTL_KERN, KERN_BOOTTIME };
	if (sysctl(mib, 2, &boot, &sz, NULL, 0) == 0 && boot.tv_sec > 0) {
		time_t now = time(NULL);
		APPEND("# HELP ps5_boot_time_seconds Unix time of the last boot.\n# TYPE ps5_boot_time_seconds gauge\nps5_boot_time_seconds %ld\n", (long)boot.tv_sec);
		APPEND("# HELP ps5_uptime_seconds Seconds since boot.\n# TYPE ps5_uptime_seconds gauge\nps5_uptime_seconds %ld\n", (long)(now - boot.tv_sec));
	}
	return len;
}

/* Direct memory is the pool games and the GPU allocate from. The kernel
   tells its size and the largest free block; the latter is a lower bound
   for what is really free, so it is named that way. */
static size_t direct_memory(char* out, size_t cap)
{
	size_t len = 0;
	size_t total = sceKernelGetDirectMemorySize();
	if (total == 0 || total == (size_t)-1) return 0;
	APPEND("# HELP ps5_direct_memory_bytes Size of the direct memory pool (games, GPU).\n# TYPE ps5_direct_memory_bytes gauge\nps5_direct_memory_bytes %zu\n", total);
	int64_t start = 0;
	size_t largest = 0;
	if (sceKernelAvailableDirectMemorySize(0, (int64_t)total, 0, &start, &largest) == 0) {
		APPEND("# HELP ps5_direct_memory_largest_free_bytes Largest free block of direct memory (lower bound of free direct memory).\n# TYPE ps5_direct_memory_largest_free_bytes gauge\nps5_direct_memory_largest_free_bytes %zu\n", largest);
	}
	return len;
}

static size_t filesystems(char* out, size_t cap)
{
	size_t len = 0;
	struct statfs* mounts = NULL;
	int n = getmntinfo(&mounts, MNT_NOWAIT);
	if (n <= 0) return 0;
	/* Samples of one metric must stay together, so two passes. */
	char mp[256], fs[64];
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 0) APPEND("# HELP ps5_filesystem_size_bytes Filesystem size.\n# TYPE ps5_filesystem_size_bytes gauge\n");
		else APPEND("# HELP ps5_filesystem_avail_bytes Filesystem space available to unprivileged users.\n# TYPE ps5_filesystem_avail_bytes gauge\n");
		for (int i = 0; i < n; i++) {
			const struct statfs* m = &mounts[i];
			if (m->f_blocks == 0) continue;
			if (strcmp(m->f_fstypename, "devfs") == 0 || strcmp(m->f_fstypename, "nullfs") == 0 ||
			    strcmp(m->f_fstypename, "tmpfs") == 0 || strcmp(m->f_fstypename, "procfs") == 0) continue;
			escape_label(m->f_mntonname, mp, sizeof(mp));
			escape_label(m->f_fstypename, fs, sizeof(fs));
			unsigned long long size = (unsigned long long)m->f_blocks * m->f_bsize;
			unsigned long long avail = (unsigned long long)(m->f_bavail < 0 ? 0 : m->f_bavail) * m->f_bsize;
			if (pass == 0) APPEND("ps5_filesystem_size_bytes{mountpoint=\"%s\",fstype=\"%s\"} %llu\n", mp, fs, size);
			else APPEND("ps5_filesystem_avail_bytes{mountpoint=\"%s\",fstype=\"%s\"} %llu\n", mp, fs, avail);
		}
	}
	return len;
}

/* The if_data layout the SDK headers describe does not match what the PS5
   kernel hands out (counters come out as small numbers), so these are only
   built with -DEXPORTER_NETWORK until the layout is confirmed. */
static size_t network(char* out, size_t cap)
{
#ifndef EXPORTER_NETWORK
	(void)out; (void)cap;
	return 0;
#else
	size_t len = 0;
	struct ifaddrs* list = NULL;
	if (getifaddrs(&list) != 0) return 0;
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 0) APPEND("# HELP ps5_network_receive_bytes_total Bytes received per interface.\n# TYPE ps5_network_receive_bytes_total counter\n");
		else APPEND("# HELP ps5_network_transmit_bytes_total Bytes sent per interface.\n# TYPE ps5_network_transmit_bytes_total counter\n");
		for (struct ifaddrs* a = list; a; a = a->ifa_next) {
			if (!a->ifa_addr || a->ifa_addr->sa_family != AF_LINK || !a->ifa_data) continue;
			if (strncmp(a->ifa_name, "lo", 2) == 0) continue;
			const struct if_data* d = (const struct if_data*)a->ifa_data;
			char name[64];
			escape_label(a->ifa_name, name, sizeof(name));
			if (pass == 0) APPEND("ps5_network_receive_bytes_total{device=\"%s\"} %llu\n", name, (unsigned long long)d->ifi_ibytes);
			else APPEND("ps5_network_transmit_bytes_total{device=\"%s\"} %llu\n", name, (unsigned long long)d->ifi_obytes);
		}
	}
	freeifaddrs(list);
	return len;
#endif
}

static int printable_name(const char* s, size_t n)
{
	size_t i = 0;
	for (; i < n && s[i]; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return 0;
	return i > 0;
}

/* kern.proc gives fixed-size kinfo_proc records: process count, and per
   process its CPU time and resident memory. The record layout is checked
   against ki_structsize so a kernel with a different layout yields only
   the count. */
static size_t processes(char* out, size_t cap)
{
	size_t len = 0;
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC };
	static unsigned char buf[512 * 1024];
	size_t sz = sizeof(buf);
	if (sysctl(mib, 3, buf, &sz, NULL, 0) != 0 || sz < sizeof(struct kinfo_proc)) return 0;
	unsigned long count = sz / sizeof(struct kinfo_proc);
	APPEND("# HELP ps5_processes Number of processes.\n# TYPE ps5_processes gauge\nps5_processes %lu\n", count);
	const struct kinfo_proc* first = (const struct kinfo_proc*)buf;
	if (first->ki_structsize != (int)sizeof(struct kinfo_proc)) return len;
	long pagesize = 16384; /* PS5 page size; getpagesize() is not available to payloads */
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 0) APPEND("# HELP ps5_process_cpu_seconds_total CPU time consumed by the process.\n# TYPE ps5_process_cpu_seconds_total counter\n");
		else APPEND("# HELP ps5_process_resident_bytes Resident memory of the process.\n# TYPE ps5_process_resident_bytes gauge\n");
		for (unsigned long i = 0; i < count; i++) {
			const struct kinfo_proc* k = (const struct kinfo_proc*)(buf + i * sizeof(struct kinfo_proc));
			if (!printable_name(k->ki_comm, COMMLEN + 1)) continue;
			char name[COMMLEN + 2], esc[2 * COMMLEN + 4];
			memcpy(name, k->ki_comm, COMMLEN + 1);
			name[COMMLEN + 1] = 0;
			escape_label(name, esc, sizeof(esc));
			if (pass == 0) APPEND("ps5_process_cpu_seconds_total{pid=\"%d\",name=\"%s\"} %.3f\n", (int)k->ki_pid, esc, (double)k->ki_runtime / 1e6);
			else APPEND("ps5_process_resident_bytes{pid=\"%d\",name=\"%s\"} %lld\n", (int)k->ki_pid, esc, (long long)k->ki_rssize * pagesize);
		}
	}
	return len;
}

static size_t info(char* out, size_t cap)
{
	size_t len = 0;
	char model[128] = { 0 };
	char model_esc[160];
	if (sceKernelGetHwModelName(model) != 0) model[0] = 0;
	escape_label(model, model_esc, sizeof(model_esc));
	/* sceKernelGetSystemSwVersion fills a 0x28 byte struct: size, then the
	   version as a string ("13.60") after an 8 byte header. */
	unsigned char sw[0x28] = { 0 };
	*(unsigned int*)sw = sizeof(sw);
	char version[32] = { 0 };
	if (sceKernelGetSystemSwVersion(sw) == 0) {
		memcpy(version, sw + 8, sizeof(version) - 1);
		version[sizeof(version) - 1] = 0;
	}
	char ver_esc[64];
	escape_label(version, ver_esc, sizeof(ver_esc));
	/* The kernel's own firmware version is packed BCD (0x13600007 = 13.60,
	   revision 7). The user-space API above can report a lower version:
	   jailbreak chains spoof it to keep update prompts away. Report both. */
	uint32_t fw = kernel_get_fw_version();
	char firmware[16];
	snprintf(firmware, sizeof(firmware), "%x.%02x", (fw >> 24) & 0xff, (fw >> 16) & 0xff);
	APPEND("# HELP ps5_info Console model, kernel firmware version and the version the system reports.\n# TYPE ps5_info gauge\nps5_info{model=\"%s\",firmware=\"%s\",system_version=\"%s\"} 1\n", model_esc, firmware, ver_esc);
	return len;
}

size_t metrics_system(char* out, size_t cap)
{
	size_t len = 0;
	len += info(out + len, cap - len);
	len += temperatures(out + len, cap - len);
	len += uptime(out + len, cap - len);
	len += direct_memory(out + len, cap - len);
	len += filesystems(out + len, cap - len);
	len += network(out + len, cap - len);
	len += processes(out + len, cap - len);
	return len;
}

size_t metrics_self(char* out, size_t cap, unsigned long scrapes, const char* version)
{
	size_t len = 0;
	APPEND("# HELP ps5_exporter_build_info Exporter version.\n# TYPE ps5_exporter_build_info gauge\nps5_exporter_build_info{version=\"%s\"} 1\n", version);
	APPEND("# HELP ps5_exporter_scrapes_total Metrics requests served.\n# TYPE ps5_exporter_scrapes_total counter\nps5_exporter_scrapes_total %lu\n", scrapes);
	return len;
}
