/* System metrics read through the console's libc and libkernel: no kernel
   offsets, so these keep working across firmware versions. */
#include "metrics.h"

#include <sys/types.h>
#include <sys/param.h>
#include <ifaddrs.h>
#include <net/if_dl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/user.h>
#include <stddef.h>
#include <time.h>

#include <ps5/kernel.h>

#include "optional.h"

/* libkernel exports used by the SDK's hwinfo sample. */
int sceKernelGetCpuTemperature(int* celsius);
int sceKernelGetSocSensorTemperature(int sensor, int* celsius);
long sceKernelGetCpuFrequency(void);
int sceKernelGetHwModelName(char* out);
int sceKernelGetSystemSwVersion(void* out);
size_t sceKernelGetDirectMemorySize(void);
int sceKernelAvailableDirectMemorySize(int64_t start, int64_t end, size_t align, int64_t* out_start, size_t* out_size);
/* Looked up on use (optional.h), after drakmor/ps5-hwinfo: sceKernelGetAppInfo(pid, out), 88 bytes with the
   title ID at offset 0x10, and get_page_table_stats(vm, type, &total, &free), ShellCore's page table counts
   (CPU type 1, GPU type 2). */

/* Collector results for ps5_exporter_collector_success. */
#define MAX_COLLECTORS 24
static struct { const char* name; int ok; } g_collectors[MAX_COLLECTORS];
static int g_collector_count;

void metrics_note_collector(const char* name, int ok)
{
	for (int i = 0; i < g_collector_count; i++) {
		if (strcmp(g_collectors[i].name, name) == 0) {
			g_collectors[i].ok = ok;
			return;
		}
	}
	if (g_collector_count < MAX_COLLECTORS) {
		g_collectors[g_collector_count].name = name;
		g_collectors[g_collector_count].ok = ok;
		g_collector_count++;
	}
}

static void escape_label(const char* in, char* out, size_t cap)
{
	size_t j = 0;
	for (size_t i = 0; in[i] && j + 2 < cap; i++) {
		char c = in[i];
		if (c == '"' || c == '\\') out[j++] = '\\';
		if (c == '\n' || c == '\r' || c == '\t') { out[j++] = ' '; continue; }
		out[j++] = c;
	}
	out[j] = 0;
}

static void temperatures(Buf* b)
{
	int t = 0;
	int samples = 0;
	size_t mark = buf_mark(b);
	buf_addf(b, "# HELP ps5_temperature_celsius Temperature reported by the console.\n# TYPE ps5_temperature_celsius gauge\n");
	if (sceKernelGetCpuTemperature(&t) == 0 && t > -100 && t < 200) {
		buf_addf(b, "ps5_temperature_celsius{sensor=\"cpu\"} %d\n", t);
		samples++;
	}
	/* Report every SoC sensor index the kernel answers for (0..15); the
	   console has more than the four the SDK sample shows. */
	for (int s = 0; s < 16; s++) {
		t = -1000;
		if (sceKernelGetSocSensorTemperature(s, &t) == 0 && t > -100 && t < 200) {
			buf_addf(b, "ps5_temperature_celsius{sensor=\"soc%d\"} %d\n", s, t);
			samples++;
		}
	}
	/* An empty family is valid but invisible in queries; drop the header
	   instead and report the failure through the collector metric. */
	if (samples == 0) buf_rewind(b, mark);
	metrics_note_collector("temperatures", samples > 0);
}

static void cpu_frequency(Buf* b)
{
	long hz = sceKernelGetCpuFrequency();
	if (hz <= 0) {
		metrics_note_collector("cpu_frequency", 0);
		return;
	}
	buf_addf(b, "# HELP ps5_cpu_frequency_hertz CPU frequency.\n# TYPE ps5_cpu_frequency_hertz gauge\nps5_cpu_frequency_hertz %ld\n", hz);
	metrics_note_collector("cpu_frequency", 1);
}

static void uptime(Buf* b)
{
	struct timeval boot;
	size_t sz = sizeof(boot);
	int mib[2] = { CTL_KERN, KERN_BOOTTIME };
	if (sysctl(mib, 2, &boot, &sz, NULL, 0) != 0 || boot.tv_sec <= 0) {
		metrics_note_collector("uptime", 0);
		return;
	}
	time_t now = time(NULL);
	buf_addf(b, "# HELP ps5_boot_time_seconds Unix time of the last boot.\n# TYPE ps5_boot_time_seconds gauge\nps5_boot_time_seconds %ld\n", (long)boot.tv_sec);
	buf_addf(b, "# HELP ps5_uptime_seconds Seconds since boot.\n# TYPE ps5_uptime_seconds gauge\nps5_uptime_seconds %ld\n", (long)(now - boot.tv_sec));
	metrics_note_collector("uptime", 1);
}

/* Direct memory is the pool games and the GPU allocate from. The kernel
   tells its size and the largest free block; the latter is a lower bound
   for what is really free, so it is named that way. */
static void direct_memory(Buf* b)
{
	size_t total = sceKernelGetDirectMemorySize();
	if (total == 0 || total == (size_t)-1) {
		metrics_note_collector("direct_memory", 0);
		return;
	}
	buf_addf(b, "# HELP ps5_direct_memory_bytes Size of the direct memory pool (games, GPU).\n# TYPE ps5_direct_memory_bytes gauge\nps5_direct_memory_bytes %zu\n", total);
	int64_t start = 0;
	size_t largest = 0;
	if (sceKernelAvailableDirectMemorySize(0, (int64_t)total, 0, &start, &largest) == 0 && largest <= total) {
		buf_addf(b, "# HELP ps5_direct_memory_largest_free_bytes Largest free block of direct memory (lower bound of free direct memory).\n# TYPE ps5_direct_memory_largest_free_bytes gauge\nps5_direct_memory_largest_free_bytes %zu\n", largest);
	}
	metrics_note_collector("direct_memory", 1);
}

static int sysctl_u64(const char* name, uint64_t* out)
{
	uint64_t v = 0;
	size_t len = sizeof(v);
	if (sysctlbyname(name, &v, &len, NULL, 0) != 0) return -1;
	*out = len == 4 ? (uint32_t)v : v;
	return 0;
}

/* What a payload may read about memory: the vm.stats.vm.* totals are refused
   on 13.60, the swap totals and the wired counts of vm0 (the system) and
   vm10 are not. Page tables as ShellCore logs them ("page table CPU 4864/9216
   GPU 179/9216"). */
static void memory(Buf* b)
{
	const uint64_t page = 16384;
	int ok = 0;
	uint64_t total = 0, avail = 0;
	if (sysctl_u64("vm.swap_total", &total) == 0 && sysctl_u64("vm.swap_avail", &avail) == 0 && total > 0 && avail * page <= total) {
		buf_addf(b, "# HELP ps5_swap_size_bytes Swap size.\n# TYPE ps5_swap_size_bytes gauge\nps5_swap_size_bytes %llu\n", (unsigned long long)total);
		buf_addf(b, "# HELP ps5_swap_free_bytes Free swap.\n# TYPE ps5_swap_free_bytes gauge\nps5_swap_free_bytes %llu\n", (unsigned long long)(avail * page));
		ok = 1;
	}
	uint64_t wired0 = 0, wired10 = 0;
	int have0 = sysctl_u64("vm.stats.vm0.v_wire_count", &wired0) == 0;
	int have10 = sysctl_u64("vm.stats.vm10.v_wire_count", &wired10) == 0;
	if (have0 || have10) {
		buf_addf(b, "# HELP ps5_memory_wired_bytes Wired memory per VM (0 is the system).\n# TYPE ps5_memory_wired_bytes gauge\n");
		if (have0) buf_addf(b, "ps5_memory_wired_bytes{vm=\"0\"} %llu\n", (unsigned long long)(wired0 * page));
		if (have10) buf_addf(b, "ps5_memory_wired_bytes{vm=\"10\"} %llu\n", (unsigned long long)(wired10 * page));
		ok = 1;
	}
	int cpu_total = -1, cpu_free = -1, gpu_total = -1, gpu_free = -1;
	int (*tables)(int, int, int*, int*) = OPTIONAL(int (*)(int, int, int*, int*), get_page_table_stats);
	int have_cpu = tables && tables(0, 1, &cpu_total, &cpu_free) == 0 && cpu_total > 0 && cpu_free >= 0 && cpu_free <= cpu_total;
	int have_gpu = tables && tables(0, 2, &gpu_total, &gpu_free) == 0 && gpu_total > 0 && gpu_free >= 0 && gpu_free <= gpu_total;
	if (have_cpu || have_gpu) {
		buf_addf(b, "# HELP ps5_page_tables Page tables as ShellCore counts them, per user (cpu, gpu) and state (total, free).\n# TYPE ps5_page_tables gauge\n");
		if (have_cpu) buf_addf(b, "ps5_page_tables{user=\"cpu\",state=\"total\"} %d\nps5_page_tables{user=\"cpu\",state=\"free\"} %d\n", cpu_total, cpu_free);
		if (have_gpu) buf_addf(b, "ps5_page_tables{user=\"gpu\",state=\"total\"} %d\nps5_page_tables{user=\"gpu\",state=\"free\"} %d\n", gpu_total, gpu_free);
		ok = 1;
	}
	metrics_note_collector("memory", ok);
}

static int skip_filesystem(const struct statfs* m)
{
	if (m->f_blocks == 0) return 1;
	return strcmp(m->f_fstypename, "devfs") == 0 || strcmp(m->f_fstypename, "nullfs") == 0 ||
	       strcmp(m->f_fstypename, "tmpfs") == 0 || strcmp(m->f_fstypename, "procfs") == 0;
}

static void filesystems(Buf* b)
{
	struct statfs* mounts = NULL;
	int n = getmntinfo(&mounts, MNT_NOWAIT);
	if (n <= 0) {
		metrics_note_collector("filesystems", 0);
		return;
	}
	/* Samples of one metric family must stay together, so one pass each.
	   free (f_bfree) and avail (f_bavail) differ by the reserved blocks;
	   used is exactly size - free. */
	static const char* heads[3] = {
		"# HELP ps5_filesystem_size_bytes Filesystem size.\n# TYPE ps5_filesystem_size_bytes gauge\n",
		"# HELP ps5_filesystem_avail_bytes Filesystem space available to unprivileged users.\n# TYPE ps5_filesystem_avail_bytes gauge\n",
		"# HELP ps5_filesystem_free_bytes Filesystem free space including reserved blocks.\n# TYPE ps5_filesystem_free_bytes gauge\n",
	};
	char mp[256], fs[64];
	for (int pass = 0; pass < 3; pass++) {
		buf_addf(b, "%s", heads[pass]);
		for (int i = 0; i < n; i++) {
			const struct statfs* m = &mounts[i];
			if (skip_filesystem(m)) continue;
			escape_label(m->f_mntonname, mp, sizeof(mp));
			escape_label(m->f_fstypename, fs, sizeof(fs));
			unsigned long long bsize = (unsigned long long)m->f_bsize;
			unsigned long long value;
			if (pass == 0) value = (unsigned long long)m->f_blocks * bsize;
			else if (pass == 1) value = (unsigned long long)(m->f_bavail < 0 ? 0 : m->f_bavail) * bsize;
			else value = (unsigned long long)m->f_bfree * bsize;
			buf_addf(b, "ps5_filesystem_%s_bytes{mountpoint=\"%s\",fstype=\"%s\"} %llu\n",
			         pass == 0 ? "size" : (pass == 1 ? "avail" : "free"), mp, fs, value);
		}
	}
	metrics_note_collector("filesystems", 1);
}

/* The if_data layout the SDK headers describe does not match what the PS5
   kernel hands out (counters come out as small numbers), so these are only
   built with -DEXPORTER_NETWORK until the layout is confirmed. */
static void network(Buf* b)
{
#ifndef EXPORTER_NETWORK
	(void)b;
#else
	struct ifaddrs* list = NULL;
	if (getifaddrs(&list) != 0) {
		metrics_note_collector("network", 0);
		return;
	}
	for (int pass = 0; pass < 2; pass++) {
		if (pass == 0) buf_addf(b, "# HELP ps5_network_receive_bytes_total Bytes received per interface.\n# TYPE ps5_network_receive_bytes_total counter\n");
		else buf_addf(b, "# HELP ps5_network_transmit_bytes_total Bytes sent per interface.\n# TYPE ps5_network_transmit_bytes_total counter\n");
		for (struct ifaddrs* a = list; a; a = a->ifa_next) {
			if (!a->ifa_addr || a->ifa_addr->sa_family != AF_LINK || !a->ifa_data) continue;
			if (strncmp(a->ifa_name, "lo", 2) == 0) continue;
			const struct if_data* d = (const struct if_data*)a->ifa_data;
			char name[64];
			escape_label(a->ifa_name, name, sizeof(name));
			if (pass == 0) buf_addf(b, "ps5_network_receive_bytes_total{device=\"%s\"} %llu\n", name, (unsigned long long)d->ifi_ibytes);
			else buf_addf(b, "ps5_network_transmit_bytes_total{device=\"%s\"} %llu\n", name, (unsigned long long)d->ifi_obytes);
		}
	}
	freeifaddrs(list);
	metrics_note_collector("network", 1);
#endif
}

/* Address, MAC and MTU per interface that has an IPv4 address. */
static void network_info(Buf* b)
{
	struct ifaddrs* list = NULL;
	if (getifaddrs(&list) != 0) {
		metrics_note_collector("network_info", 0);
		return;
	}
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	int samples = 0;
	size_t mark = buf_mark(b);
	buf_addf(b, "# HELP ps5_network_info Network interfaces with an IPv4 address.\n# TYPE ps5_network_info gauge\n");
	for (struct ifaddrs* a = list; a; a = a->ifa_next) {
		if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
		if (strncmp(a->ifa_name, "lo", 2) == 0) continue;
		char address[INET_ADDRSTRLEN] = "";
		inet_ntop(AF_INET, &((struct sockaddr_in*)a->ifa_addr)->sin_addr, address, sizeof(address));
		char mac[18] = "";
		for (struct ifaddrs* l = list; l; l = l->ifa_next) {
			if (!l->ifa_addr || l->ifa_addr->sa_family != AF_LINK || strcmp(l->ifa_name, a->ifa_name) != 0) continue;
			const struct sockaddr_dl* dl = (const struct sockaddr_dl*)l->ifa_addr;
			if (dl->sdl_alen == 6) {
				const unsigned char* m = (const unsigned char*)LLADDR(dl);
				snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
			}
		}
		int mtu = 0;
		if (sock >= 0) {
			struct ifreq req;
			memset(&req, 0, sizeof(req));
			strncpy(req.ifr_name, a->ifa_name, sizeof(req.ifr_name) - 1);
			if (ioctl(sock, SIOCGIFMTU, &req) == 0) mtu = req.ifr_mtu;
		}
		char name[64];
		escape_label(a->ifa_name, name, sizeof(name));
		buf_addf(b, "ps5_network_info{device=\"%s\",address=\"%s\",mac=\"%s\",mtu=\"%d\"} 1\n", name, address, mac, mtu);
		samples++;
	}
	if (sock >= 0) close(sock);
	freeifaddrs(list);
	if (samples == 0) buf_rewind(b, mark);
	metrics_note_collector("network_info", samples > 0);
}

static int printable_name(const char* s, size_t n)
{
	size_t i = 0;
	for (; i < n && s[i]; i++) if ((unsigned char)s[i] < 32 || (unsigned char)s[i] > 126) return 0;
	return i > 0;
}

/* Per-executable totals. Aggregating by name instead of pid keeps the time
   series bounded: a pid label would create a new series for every process
   the console ever starts, and rate() cannot work on series that live for a
   few seconds. The trade-off is that the counter drops when a process with
   that name exits, which Prometheus reads as a counter reset. */
#define MAX_PROC_NAMES 128
struct proc_agg {
	char name[COMMLEN + 2];
	double cpu_seconds;
	unsigned long long resident;
	int count;
};

static void processes(Buf* b)
{
	int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_PROC };
	static unsigned char buf[512 * 1024];
	size_t sz = sizeof(buf);
	/* A short read (ENOMEM) still fills the buffer with whole records, so
	   use what we got instead of dropping everything. */
	int rc = sysctl(mib, 3, buf, &sz, NULL, 0);
	if ((rc != 0 && sz == 0) || sz < sizeof(struct kinfo_proc)) {
		metrics_note_collector("processes", 0);
		return;
	}
	/* The kernel states its own record size in the first field. The PS5
	   layout does not match the SDK header, so trust the kernel for the
	   stride and only read fields up to the size it reports. */
	const struct kinfo_proc* first = (const struct kinfo_proc*)buf;
	size_t stride = (size_t)first->ki_structsize;
	/* The record layout differs per firmware, so publish what the kernel
	   reports: that turns a silent "no process details" into something you
	   can actually see and fix. */
	buf_addf(b, "# HELP ps5_process_record_bytes Size of one process record as the kernel reports it (0 when unusable).\n# TYPE ps5_process_record_bytes gauge\nps5_process_record_bytes %zu\n", stride);
	if (stride < offsetof(struct kinfo_proc, ki_comm) + COMMLEN + 1 || stride > sz) {
		/* Unusable layout: report the count from the SDK-sized stride only. */
		buf_addf(b, "# HELP ps5_processes Number of processes.\n# TYPE ps5_processes gauge\nps5_processes %lu\n", (unsigned long)(sz / sizeof(struct kinfo_proc)));
		metrics_note_collector("processes", 1);
		metrics_note_collector("process_details", 0);
		return;
	}
	unsigned long count = sz / stride;
	buf_addf(b, "# HELP ps5_processes Number of processes.\n# TYPE ps5_processes gauge\nps5_processes %lu\n", count);
	static struct proc_agg agg[MAX_PROC_NAMES];
	int nagg = 0;
	char title[16] = "";
	const long pagesize = 16384; /* PS5 page size; getpagesize() is unavailable to payloads */
	for (unsigned long i = 0; i < count; i++) {
		const struct kinfo_proc* k = (const struct kinfo_proc*)(buf + i * stride);
		if (!printable_name(k->ki_comm, COMMLEN + 1)) continue;
		char name[COMMLEN + 2];
		memcpy(name, k->ki_comm, COMMLEN + 1);
		name[COMMLEN + 1] = 0;
		int slot = -1;
		for (int j = 0; j < nagg; j++) if (strcmp(agg[j].name, name) == 0) { slot = j; break; }
		if (slot < 0) {
			if (nagg >= MAX_PROC_NAMES) continue;
			slot = nagg++;
			memcpy(agg[slot].name, name, sizeof(name));
			agg[slot].cpu_seconds = 0;
			agg[slot].resident = 0;
			agg[slot].count = 0;
		}
		/* A game runs as eboot.bin; its app info holds the title ID at 0x10. */
		if (title[0] == 0 && strcmp(name, "eboot.bin") == 0) {
			unsigned char app[128];
			memset(app, 0, sizeof(app));
			int (*app_info)(int, void*) = OPTIONAL(int (*)(int, void*), sceKernelGetAppInfo);
			if (app_info && app_info(k->ki_pid, app) == 0) {
				memcpy(title, app + 0x10, 9);
				title[9] = 0;
				if (!printable_name(title, 9)) title[0] = 0;
			}
		}
		agg[slot].cpu_seconds += (double)k->ki_runtime / 1e6;
		agg[slot].resident += (unsigned long long)(k->ki_rssize < 0 ? 0 : k->ki_rssize) * (unsigned long long)pagesize;
		agg[slot].count++;
	}
	for (int pass = 0; pass < 3; pass++) {
		if (pass == 0) buf_addf(b, "# HELP ps5_process_cpu_seconds_total CPU time of all processes with this name. Drops when such a process exits, which reads as a counter reset.\n# TYPE ps5_process_cpu_seconds_total counter\n");
		else if (pass == 1) buf_addf(b, "# HELP ps5_process_resident_bytes Resident memory of all processes with this name.\n# TYPE ps5_process_resident_bytes gauge\n");
		else buf_addf(b, "# HELP ps5_process_instances Number of processes with this name.\n# TYPE ps5_process_instances gauge\n");
		for (int j = 0; j < nagg; j++) {
			char esc[2 * COMMLEN + 4];
			escape_label(agg[j].name, esc, sizeof(esc));
			if (pass == 0) buf_addf(b, "ps5_process_cpu_seconds_total{name=\"%s\"} %.3f\n", esc, agg[j].cpu_seconds);
			else if (pass == 1) buf_addf(b, "ps5_process_resident_bytes{name=\"%s\"} %llu\n", esc, agg[j].resident);
			else buf_addf(b, "ps5_process_instances{name=\"%s\"} %d\n", esc, agg[j].count);
		}
	}
	metrics_note_collector("processes", 1);
	metrics_note_collector("process_details", 1);
	char esc[32];
	escape_label(title, esc, sizeof(esc));
	buf_addf(b, "# HELP ps5_game_running Whether a game runs (eboot.bin).\n# TYPE ps5_game_running gauge\nps5_game_running %d\n", title[0] != 0);
	if (title[0]) buf_addf(b, "# HELP ps5_game_info The running game's title ID.\n# TYPE ps5_game_info gauge\nps5_game_info{title_id=\"%s\"} 1\n", esc);
}

static void info(Buf* b)
{
	char model[128] = { 0 };
	char model_esc[160];
	if (sceKernelGetHwModelName(model) != 0) model[0] = 0;
	escape_label(model, model_esc, sizeof(model_esc));
	/* sceKernelGetSystemSwVersion fills a 0x28 byte struct: size, then the
	   version as a string after an 8 byte header. */
	unsigned char sw[0x28] = { 0 };
	unsigned int sw_size = sizeof(sw);
	memcpy(sw, &sw_size, sizeof(sw_size));
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
	buf_addf(b, "# HELP ps5_info Console model, kernel firmware version and the version the system reports.\n# TYPE ps5_info gauge\nps5_info{model=\"%s\",firmware=\"%s\",system_version=\"%s\"} 1\n", model_esc, firmware, ver_esc);
	metrics_note_collector("info", model_esc[0] != 0 || fw != 0);
}

void metrics_system(Buf* b)
{
	info(b);
	temperatures(b);
	cpu_frequency(b);
	uptime(b);
	direct_memory(b);
	memory(b);
	filesystems(b);
	network(b);
	network_info(b);
	processes(b);
}

void metrics_self(Buf* b, unsigned long scrapes, const char* version)
{
	buf_addf(b, "# HELP ps5_exporter_build_info Exporter version.\n# TYPE ps5_exporter_build_info gauge\nps5_exporter_build_info{version=\"%s\"} 1\n", version);
	buf_addf(b, "# HELP ps5_exporter_scrapes_total Metrics requests served.\n# TYPE ps5_exporter_scrapes_total counter\nps5_exporter_scrapes_total %lu\n", scrapes);
	if (g_collector_count > 0) {
		buf_addf(b, "# HELP ps5_exporter_collector_success Whether a collector produced data during the last scrape.\n# TYPE ps5_exporter_collector_success gauge\n");
		for (int i = 0; i < g_collector_count; i++) {
			buf_addf(b, "ps5_exporter_collector_success{collector=\"%s\"} %d\n", g_collectors[i].name, g_collectors[i].ok);
		}
	}
}
