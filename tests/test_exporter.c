/* Host tests for the parts that do not need a console: the append buffer,
 * the sensor range checks and the ShadowMount JSON parser. The kernel and
 * HTTP calls are faked here, so this runs anywhere with gcc.
 *
 * Built by `make test` with AddressSanitizer and UBSan enabled, which is
 * what catches the buffer bugs this file is mainly here to prevent. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/buf.h"
#include "../src/sensors.h"
#include "../src/shadowmount.h"

static int g_failures;

static void ok(const char* what, int condition)
{
	printf("%-52s %s\n", what, condition ? "ok" : "FAIL");
	if (!condition) g_failures++;
}

/* ---------------------------------------------------------- fake kernel */

int g_fan_rc = 0;
uint16_t g_duty = 512;
int g_cpu_rc = 0, g_cores = 8, g_core_val = 42;
int g_pow_rc = 0;
uint64_t g_pow = 45000;
int g_time_rc = 0, g_cycles_rc = 0;
uint64_t g_time = 360000, g_cycles = 420;

int sceKernelGetCurrentFanDuty(uint16_t* duty, uint64_t* chassis) { *duty = g_duty; *chassis = 1; return g_fan_rc; }
int sceKernelGetCpuUsageAll(int* per_core, int* count)
{
	int n = g_cores > 32 ? 32 : g_cores;
	for (int i = 0; i < n; i++) per_core[i] = g_core_val;
	*count = g_cores;
	return g_cpu_rc;
}
int sceKernelGetSocPowerConsumption(uint64_t* out, double r) { (void)r; out[0] = g_pow; return g_pow_rc; }
int sceKernelIccGetPowerNumberOfBootShutdown(uint64_t* out) { *out = g_cycles; return g_cycles_rc; }
int sceKernelIccGetPowerOperatingTime(uint64_t* out) { *out = g_time; return g_time_rc; }
int sceKernelGetBasicProductShape(int* out) { *out = 1; return 0; }

/* ------------------------------------------------------------ fake HTTP */

static const char* g_version_body = "{\"status\":0,\"shadowmount_version\":\"1.7\"}";
static const char* g_storage_body = "{\"status\":0,\"mounts\":[]}";
static const char* g_games_body = "{\"status\":0,\"count\":0,\"games\":[]}";
static int g_http_status = 200;

int http_post_json(const char* host, int port, const char* path, const char* json,
                   char* out, size_t out_cap, int timeout_ms)
{
	(void)host; (void)port; (void)json; (void)timeout_ms;
	const char* body = g_version_body;
	if (strstr(path, "storage")) body = g_storage_body;
	else if (strstr(path, "games")) body = g_games_body;
	snprintf(out, out_cap, "%s", body);
	return g_http_status;
}

/* ------------------------------------------------------------ buf tests */

static void test_buf(void)
{
	char storage[32];
	Buf b;
	buf_init(&b, storage, sizeof(storage));
	buf_addf(&b, "hello ");
	buf_addf(&b, "world");
	ok("buf: appends", strcmp(storage, "hello world") == 0 && b.len == 11 && !b.truncated);

	/* An append that does not fit must be dropped whole, not half-written. */
	buf_addf(&b, "%s", "0123456789012345678901234567890123456789");
	ok("buf: oversized append dropped", strcmp(storage, "hello world") == 0);
	ok("buf: truncation flagged", b.truncated);
	ok("buf: len still inside capacity", b.len < b.cap);

	/* Once full, further appends stay harmless. */
	for (int i = 0; i < 100; i++) buf_addf(&b, "x");
	ok("buf: stays inside capacity when full", b.len < b.cap && storage[sizeof(storage) - 1] == 0 ? 1 : b.len < b.cap);

	/* Exact fit: 31 characters plus the terminator in a 32 byte buffer. */
	Buf c;
	char small[32];
	buf_init(&c, small, sizeof(small));
	buf_addf(&c, "%031d", 0);
	ok("buf: exact fit accepted", c.len == 31 && !c.truncated);

	/* One character more than fits must be refused. */
	Buf d;
	char tiny[8];
	buf_init(&d, tiny, sizeof(tiny));
	buf_addf(&d, "%08d", 0);
	ok("buf: one byte too many refused", d.len == 0 && d.truncated);

	/* mark/rewind drops a partially written family. */
	Buf e;
	char mark_buf[64];
	buf_init(&e, mark_buf, sizeof(mark_buf));
	buf_addf(&e, "keep");
	size_t mark = buf_mark(&e);
	buf_addf(&e, "drop this");
	buf_rewind(&e, mark);
	ok("buf: rewind drops the tail", strcmp(mark_buf, "keep") == 0 && e.len == 4);
}

/* -------------------------------------------------------- sensor tests */

static int sensors_contains(const char* needle)
{
	static char storage[16384];
	Buf b;
	buf_init(&b, storage, sizeof(storage));
	metrics_sensors(&b);
	return strstr(storage, needle) != NULL;
}

static void test_sensors(void)
{
	g_duty = 512;
	ok("fan: 512/1024 becomes ratio 0.5", sensors_contains("ps5_fan_duty_ratio 0.5000\n"));
	g_duty = 2000;
	ok("fan: out of range dropped", !sensors_contains("ps5_fan_duty_ratio"));
	g_duty = 512;
	g_fan_rc = -1;
	ok("fan: failed call dropped", !sensors_contains("ps5_fan_duty_ratio"));
	g_fan_rc = 0;

	ok("cpu: per core ratio", sensors_contains("ps5_cpu_usage_ratio{core=\"7\"} 0.42\n"));
	ok("cpu: no aggregate inside the family", !sensors_contains("core=\"all\""));
	g_core_val = 250;
	ok("cpu: bogus value drops the family", !sensors_contains("ps5_cpu_usage_ratio{core="));
	g_core_val = 42;
	g_cores = 99;
	ok("cpu: bogus core count dropped", !sensors_contains("ps5_cpu_cores"));
	g_cores = 8;

	ok("power: plausible value becomes watts", sensors_contains("ps5_soc_power_watts 45.00\n"));
	g_pow = 7;
	ok("power: implausible value has no watts", !sensors_contains("ps5_soc_power_watts"));
	ok("power: raw value still reported", sensors_contains("ps5_soc_power_raw_value 7\n"));
	g_pow = 45000;

	ok("lifetime: operating time", sensors_contains("ps5_power_operating_seconds_total 360000\n"));
	ok("lifetime: power cycles", sensors_contains("ps5_power_cycles_total 420\n"));
	g_time = (uint64_t)200 * 365 * 24 * 3600;
	ok("lifetime: implausible runtime dropped", !sensors_contains("ps5_power_operating_seconds_total"));
	g_time = 360000;
}

/* ---------------------------------------------------- shadowmount tests */

static void collect_shadowmount(char* storage, size_t cap, Buf* b)
{
	/* Each case starts from an empty cache, otherwise a previous case's
	   game list would be served instead of the one under test. */
	metrics_shadowmount_reset_cache();
	buf_init(b, storage, cap);
	metrics_shadowmount(b);
}

static void test_shadowmount_malformed(void)
{
	static const char* bodies[] = {
		"{}",
		"{\"shadowmount_version\":\"1.7",                       /* truncated string */
		"{\"shadowmount_version\":\"abc\\",                      /* escape at the end */
		"{\"shadowmount_version\":\"abc\\u12",                   /* short unicode escape */
		"{\"mounts\":[{\"mount_point\":\"/a\",\"total_bytes\":1", /* unterminated array */
		"{\"games\":[{\"title_id\":\"A}B\",\"title_name\":\"x{y\",\"mounted\":true}]}",
		"{\"games\":[{\"title_id\":\"A\",\"title_name\":\"he said \\\"hi\\\"\",\"mounted\":true}]}",
		"{\"games\":[{\"title_id\":\"A\",\"title_name\":\"ends with backslash\\\\\",\"mounted\":true}]}",
		"{\"mounts\":[{\"total_bytes\":}]}",
		"{\"mounts\":[{\"mount_point\":\"/a\",\"total_bytes\":999999999999999999999999}]}",
		"{\"games\":[{\"title_id\":\"A\"",
	};
	int clean = 1;
	for (size_t i = 0; i < sizeof(bodies) / sizeof(bodies[0]); i++) {
		char* storage = malloc(16384); /* heap so ASan guards both ends */
		Buf b;
		g_version_body = bodies[i];
		g_storage_body = bodies[i];
		g_games_body = bodies[i];
		collect_shadowmount(storage, 16384, &b);
		if (b.len >= b.cap) clean = 0;
		free(storage);
	}
	ok("json: malformed input stays inside the buffer", clean);
	g_version_body = "{\"status\":0,\"shadowmount_version\":\"1.7\"}";
	g_storage_body = "{\"status\":0,\"mounts\":[]}";
	g_games_body = "{\"status\":0,\"count\":0,\"games\":[]}";
}

/* The bug this file exists for: a game list far larger than the output
   buffer used to overflow it. */
static void test_shadowmount_overflow(void)
{
	static char big[400000];
	strcpy(big, "{\"status\":0,\"count\":900,\"games\":[");
	for (int i = 0; i < 900; i++) {
		char item[256];
		snprintf(item, sizeof(item),
		         "%s{\"title_id\":\"PPSA%05d\",\"title_name\":\"Game with a fairly long name %d\",\"platform\":\"ps5\",\"source_type\":\"folder\",\"mounted\":false,\"installed\":true,\"source_available\":true}",
		         i ? "," : "", i, i);
		strcat(big, item);
	}
	strcat(big, "]}");
	g_games_body = big;

	char* storage = malloc(64 * 1024);
	Buf b;
	collect_shadowmount(storage, 64 * 1024, &b);
	ok("overflow: 900 games stay inside a 64 KB buffer", b.len < b.cap);
	ok("overflow: output stays NUL-terminated", storage[b.len] == 0);
	/* 900 games produce far more than 64 KB of metrics, so the exporter must
	   notice it could not emit everything (main.c then answers 500 rather
	   than serving half a scrape). */
	ok("overflow: truncation is reported", b.truncated);
	free(storage);

	/* A buffer too small even for the first lines must stay safe as well. */
	char* tiny = malloc(100);
	Buf t;
	collect_shadowmount(tiny, 100, &t);
	ok("overflow: tiny buffer stays inside bounds", t.len < t.cap && tiny[t.len] == 0);
	ok("overflow: tiny buffer reports truncation", t.truncated);
	free(tiny);
	g_games_body = "{\"status\":0,\"count\":0,\"games\":[]}";
}

static void test_shadowmount_down(void)
{
	g_http_status = 500;
	char storage[4096];
	Buf b;
	collect_shadowmount(storage, sizeof(storage), &b);
	ok("down: reports ps5_shadowmount_up 0", strstr(storage, "ps5_shadowmount_up 0") != NULL);
	g_http_status = 200;
}

int main(void)
{
	test_buf();
	test_sensors();
	test_shadowmount_malformed();
	test_shadowmount_overflow();
	test_shadowmount_down();
	printf("\n%s (%d failure%s)\n", g_failures ? "FAIL" : "PASS", g_failures, g_failures == 1 ? "" : "s");
	return g_failures != 0;
}
