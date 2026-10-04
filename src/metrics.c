/* System metrics read through the console's libc and libkernel: no kernel
   offsets, so these keep working across firmware versions. */
#include "metrics.h"

#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <time.h>

/* libkernel exports used by the SDK's hwinfo sample. */
int sceKernelGetCpuTemperature(int* celsius);
int sceKernelGetSocSensorTemperature(int sensor, int* celsius);
long sceKernelGetCpuFrequency(void);
int sceKernelGetHwModelName(char* out);
int sceKernelGetSystemSwVersion(void* out);

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
	for (int s = 0; s < 4; s++) {
		if (sceKernelGetSocSensorTemperature(s, &t) == 0) APPEND("ps5_temperature_celsius{sensor=\"soc%d\"} %d\n", s, t);
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

static size_t memory(char* out, size_t cap)
{
	size_t len = 0;
	unsigned long physmem = 0, usermem = 0;
	size_t sz = sizeof(physmem);
	if (sysctlbyname("hw.physmem", &physmem, &sz, NULL, 0) == 0 && physmem) {
		APPEND("# HELP ps5_memory_physical_bytes Physical memory reported by hw.physmem.\n# TYPE ps5_memory_physical_bytes gauge\nps5_memory_physical_bytes %lu\n", physmem);
	}
	sz = sizeof(usermem);
	if (sysctlbyname("hw.usermem", &usermem, &sz, NULL, 0) == 0 && usermem) {
		APPEND("# HELP ps5_memory_user_bytes Memory available to user processes (hw.usermem).\n# TYPE ps5_memory_user_bytes gauge\nps5_memory_user_bytes %lu\n", usermem);
	}
	unsigned int pages = 0;
	sz = sizeof(pages);
	if (sysctlbyname("vm.stats.vm.v_free_count", &pages, &sz, NULL, 0) == 0) {
		unsigned long pagesize = 0;
		size_t psz = sizeof(pagesize);
		if (sysctlbyname("hw.pagesize", &pagesize, &psz, NULL, 0) != 0 || !pagesize) pagesize = 16384;
		APPEND("# HELP ps5_memory_free_bytes Free pages times page size (vm.stats.vm.v_free_count).\n# TYPE ps5_memory_free_bytes gauge\nps5_memory_free_bytes %lu\n", (unsigned long)pages * pagesize);
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

static size_t network(char* out, size_t cap)
{
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
}

static size_t processes(char* out, size_t cap)
{
	size_t len = 0;
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC };
	size_t sz = 0;
	if (sysctl(mib, 3, NULL, &sz, NULL, 0) == 0 && sz > 0) {
		/* kinfo_proc records are fixed size (kern.proc returns an array). */
		unsigned long count = sz / sizeof(struct kinfo_proc);
		APPEND("# HELP ps5_processes Number of processes.\n# TYPE ps5_processes gauge\nps5_processes %lu\n", count);
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
	APPEND("# HELP ps5_info Console model and system software version.\n# TYPE ps5_info gauge\nps5_info{model=\"%s\",system_version=\"%s\"} 1\n", model_esc, ver_esc);
	return len;
}

size_t metrics_system(char* out, size_t cap)
{
	size_t len = 0;
	len += info(out + len, cap - len);
	len += temperatures(out + len, cap - len);
	len += uptime(out + len, cap - len);
	len += memory(out + len, cap - len);
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
