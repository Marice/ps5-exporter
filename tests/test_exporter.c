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
#include "../src/optional.h"
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
int g_pow_rc = 0;
/* 8 rails of {mW, mV, mA}: rail r draws (r + 1) W at 1 V, rail 7 is zero. */
uint32_t g_rails[32];
int g_clock_rc = 0;
uint32_t g_gpu_mhz = 2150;
int g_core_mhz = 3200;
uint8_t g_usb = 1, g_bd = 0;
int g_time_rc = 0, g_cycles_rc = 0;
uint64_t g_time = 360000, g_cycles = 420;
int g_missing; /* every optional function is "not exported" */

static int fake_fan(uint16_t* duty, uint64_t* chassis) { *duty = g_duty; *chassis = 1; return g_fan_rc; }
static int fake_power(void* out)
{
	/* The kernel writes the whole block, whatever the caller expects. */
	memcpy(out, g_rails, 0x70);
	return g_pow_rc;
}
static int fake_soc_clock(uint32_t* out)
{
	for (int i = 0; i < 26; i++) out[i] = 500;
	out[20] = g_gpu_mhz;
	return g_clock_rc;
}
static int fake_core_clock(int* out)
{
	for (int i = 0; i < 8; i++) out[i] = g_core_mhz;
	return g_clock_rc;
}
static int fake_usb(uint8_t* out) { *out = g_usb; return 0; }
static int fake_bd(uint8_t* out) { *out = g_bd; return 0; }
static int fake_cycles(uint64_t* out) { *out = g_cycles; return g_cycles_rc; }
static int fake_time(uint64_t* out) { *out = g_time; return g_time_rc; }

void* optional_symbol(const char* name)
{
	if (g_missing) return NULL;
	static const struct { const char* name; void* fn; } table[] = {
		{ "sceKernelGetCurrentFanDuty", (void*)fake_fan },
		{ "sceKernelGetSocPowerConsumption", (void*)fake_power },
		{ "sceKernelGetSocClock", (void*)fake_soc_clock },
		{ "sceKernelGetCpuCoreClock", (void*)fake_core_clock },
		{ "sceKernelIccGetUSBPowerState", (void*)fake_usb },
		{ "sceKernelIccGetBDPowerState", (void*)fake_bd },
		{ "sceKernelIccGetPowerNumberOfBootShutdown", (void*)fake_cycles },
		{ "sceKernelIccGetPowerOperatingTime", (void*)fake_time },
	};
	for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++)
		if (strcmp(table[i].name, name) == 0) return table[i].fn;
	return NULL;
}

static void reset_rails(void)
{
	memset(g_rails, 0, sizeof(g_rails));
	for (int r = 0; r < 7; r++) {
		g_rails[r * 3] = (uint32_t)(r + 1) * 1000;
		g_rails[r * 3 + 1] = 1000;
		g_rails[r * 3 + 2] = (uint32_t)(r + 1) * 1000;
	}
}

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
	reset_rails();
	g_duty = 512;
	ok("fan: 512/1024 becomes ratio 0.5", sensors_contains("ps5_fan_duty_ratio 0.5000\n"));
	g_duty = 2000;
	ok("fan: out of range dropped", !sensors_contains("ps5_fan_duty_ratio"));
	g_duty = 512;
	g_fan_rc = -1;
	ok("fan: failed call dropped", !sensors_contains("ps5_fan_duty_ratio"));
	g_fan_rc = 0;

	ok("power: rail 0 is the GPU core", sensors_contains("ps5_power_rail_watts{rail=\"0\",name=\"gpu_core\"} 1.000\n"));
	ok("power: volts per rail", sensors_contains("ps5_power_rail_volts{rail=\"2\",name=\"cpu_soc\"} 1.000\n"));
	ok("power: amperes per rail", sensors_contains("ps5_power_rail_amperes{rail=\"6\",name=\"memory_ch45\"} 7.000\n"));
	ok("power: a zero rail is left out", !sensors_contains("rail=\"7\""));
	ok("power: total is the sum (1+..+7 W)", sensors_contains("ps5_power_watts 28.000\n"));
	ok("power: v0.2.0 raw value kept", sensors_contains("ps5_soc_power_raw_value 4294967297000\n"));
	g_rails[3] = 900000;
	ok("power: implausible rail dropped", !sensors_contains("rail=\"1\""));
	ok("power: ... and left out of the total", sensors_contains("ps5_power_watts 26.000\n"));
	reset_rails();
	g_pow_rc = -1;
	ok("power: failed call dropped", !sensors_contains("ps5_power_watts"));
	g_pow_rc = 0;

	ok("clocks: GPU clock in hertz", sensors_contains("ps5_soc_clock_hertz{domain=\"20\",name=\"gpu\"} 2150000000\n"));
	ok("clocks: per CPU core", sensors_contains("ps5_cpu_core_clock_hertz{core=\"7\"} 3200000000\n"));
	g_gpu_mhz = 50000;
	ok("clocks: implausible domain dropped", !sensors_contains("domain=\"20\""));
	g_gpu_mhz = 2150;

	ok("device power: USB on", sensors_contains("ps5_usb_power_on 1\n"));
	ok("device power: disc drive off", sensors_contains("ps5_disc_drive_power_on 0\n"));
	g_usb = 7;
	ok("device power: unknown state dropped", !sensors_contains("ps5_usb_power_on"));
	g_usb = 1;

	ok("lifetime: operating time", sensors_contains("ps5_power_operating_seconds_total 360000\n"));
	ok("lifetime: power cycles", sensors_contains("ps5_power_cycles_total 420\n"));
	g_time = (uint64_t)200 * 365 * 24 * 3600;
	ok("lifetime: implausible runtime dropped", !sensors_contains("ps5_power_operating_seconds_total"));
	g_time = 360000;

	g_missing = 1;
	ok("missing functions: no metrics, no crash", !sensors_contains("ps5_power_watts") && !sensors_contains("ps5_fan"));
	g_missing = 0;
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
