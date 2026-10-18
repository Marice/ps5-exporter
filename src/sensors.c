/* Hardware sensors beyond the temperatures: fan, per-core CPU usage, power
 * rails, clocks, the USB and disc drive power state and lifetime counters.
 *
 * These libkernel functions are not in the SDK headers. The signatures come
 * from two public homebrew projects that call them on real consoles:
 *   - sceKernelGetCurrentFanDuty(uint16_t* duty, uint64_t* chassis), duty on
 *     a 0..1024 scale: drakmor/fan_target (the ShadowMountPlus author).
 *   - CPU usage moved to cpuload.c: sceKernelGetCpuUsageAll (from
 *     aloksaurabh/elf-arsenal) answers 0x80020016 on 13.60.
 *   - sceKernelGetSocPowerConsumption(void* out) fills a 0x70-byte block: 8
 *     rails of {mW, mV, mA}, then auxiliary words. It takes ONE argument and
 *     always writes 112 bytes, so the buffer must hold at least that
 *     (phantomptr/ps5upload hung consoles with a smaller one). Layout and
 *     rail names: drakmor/ps5-hwinfo; P = V x I holds for every rail
 *     (checked on a 13.60 Pro, and by erickdavestech/ps5-hud-dualsense-overlay
 *     on a Slim). Rail 0 follows the GPU's load (1.4 W in the menu, ~96 W in a
 *     game); the other names are the community's and not confirmed.
 *   - sceKernelGetSocClock(uint32_t* domains), MHz per clock domain (index 20
 *     is the GPU clock: 900 MHz in the menu, ~2150 MHz in a game), and
 *     sceKernelGetCpuCoreClock(int* mhz_per_core): drakmor/ps5-hwinfo.
 *   - sceKernelIccGetUSBPowerState / sceKernelIccGetBDPowerState(uint8_t*):
 *     drakmor/ps5-hwinfo.
 * Every value is range-checked before it becomes a metric, so a console that
 * refuses a call or returns something unexpected simply omits that metric
 * instead of publishing nonsense. */
#include "sensors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "metrics.h"
#include "optional.h"

int sceKernelGetBasicProductShape(int* out);

#define FAN_DUTY_SCALE 1024.0
#define MAX_CORES 16

static void fan(Buf* b)
{
	uint16_t duty = 0xffff;
	uint64_t chassis = 0;
	int (*get)(uint16_t*, uint64_t*) = OPTIONAL(int (*)(uint16_t*, uint64_t*), sceKernelGetCurrentFanDuty);
	if (!get || get(&duty, &chassis) != 0 || duty > FAN_DUTY_SCALE) {
		metrics_note_collector("fan", 0);
		return;
	}
	/* One canonical representation: a 0..1 ratio, the Prometheus convention
	   for a bounded fraction. Grafana renders it as a percentage. */
	buf_addf(b, "# HELP ps5_fan_duty_ratio Fan duty cycle, 0 to 1 (kernel reports 0..1024).\n# TYPE ps5_fan_duty_ratio gauge\nps5_fan_duty_ratio %.4f\n", duty / FAN_DUTY_SCALE);
	metrics_note_collector("fan", 1);
}

/* The block is 0x70 bytes; 128 leaves headroom. */
#define POWER_WORDS 32
#define POWER_RAILS 8
#define SOC_CLOCK_SLOTS 64
#define SOC_CLOCK_DOMAINS 26

static const char* const RAIL_NAMES[POWER_RAILS] = {
	"gpu_core", "gpu_io", "cpu_soc", "cpu_io", "memory_ch01", "memory_ch23", "memory_ch45", "memory_ch67",
};

static void power(Buf* b)
{
	uint32_t w[POWER_WORDS];
	memset(w, 0, sizeof(w));
	int (*get)(void*) = OPTIONAL(int (*)(void*), sceKernelGetSocPowerConsumption);
	if (!get || get(w) != 0) {
		metrics_note_collector("power", 0);
		return;
	}
	/* Kept for PS5LM and other readers of v0.2.0: the first 64-bit word
	   (rail 0's milliwatts, then its millivolts in the high half). */
	uint64_t first = (uint64_t)w[0] | ((uint64_t)w[1] << 32);
	buf_addf(b, "# HELP ps5_soc_power_raw_value Deprecated: first 64-bit word of sceKernelGetSocPowerConsumption (rail 0's mW, then its mV). Use ps5_power_rail_watts.\n# TYPE ps5_soc_power_raw_value gauge\nps5_soc_power_raw_value %llu\n", (unsigned long long)first);
	/* A rail counts when its numbers are plausible for a console: up to 400 W,
	   3 V and 400 A each. All three families list the same rails. */
	int valid[POWER_RAILS];
	int rails = 0;
	unsigned long long total_mw = 0;
	for (int r = 0; r < POWER_RAILS; r++) {
		uint32_t mw = w[r * 3], mv = w[r * 3 + 1], ma = w[r * 3 + 2];
		valid[r] = mw <= 400000 && mv <= 3000 && ma <= 400000 && (mw || mv || ma);
		if (valid[r]) {
			rails++;
			total_mw += mw;
		}
	}
	if (rails == 0) {
		metrics_note_collector("power", 0);
		return;
	}
	static const char* const heads[3] = {
		"# HELP ps5_power_rail_watts Power per rail of sceKernelGetSocPowerConsumption. rail 0 (gpu_core) follows the GPU; the other names are the community's.\n# TYPE ps5_power_rail_watts gauge\n",
		"# HELP ps5_power_rail_volts Voltage per rail.\n# TYPE ps5_power_rail_volts gauge\n",
		"# HELP ps5_power_rail_amperes Current per rail.\n# TYPE ps5_power_rail_amperes gauge\n",
	};
	static const char* const names[3] = { "watts", "volts", "amperes" };
	for (int f = 0; f < 3; f++) {
		buf_addf(b, "%s", heads[f]);
		for (int r = 0; r < POWER_RAILS; r++) {
			if (!valid[r]) continue;
			buf_addf(b, "ps5_power_rail_%s{rail=\"%d\",name=\"%s\"} %.3f\n", names[f], r, RAIL_NAMES[r], w[r * 3 + f] / 1000.0);
		}
	}
	buf_addf(b, "# HELP ps5_power_watts Sum of the power rails (SoC and memory). Not checked against a wall meter.\n# TYPE ps5_power_watts gauge\nps5_power_watts %.3f\n", total_mw / 1000.0);
	metrics_note_collector("power", 1);
}

static void clocks(Buf* b)
{
	uint32_t soc[SOC_CLOCK_SLOTS];
	memset(soc, 0, sizeof(soc));
	int ok = 0;
	int (*get_soc)(uint32_t*) = OPTIONAL(int (*)(uint32_t*), sceKernelGetSocClock);
	if (get_soc && get_soc(soc) == 0) {
		size_t mark = buf_mark(b);
		int samples = 0;
		buf_addf(b, "# HELP ps5_soc_clock_hertz SoC clock domains. domain 20 (gpu) is the GPU clock; the names of 23 (memory) and 24 (fabric) follow drakmor/ps5-hwinfo.\n# TYPE ps5_soc_clock_hertz gauge\n");
		for (int d = 0; d < SOC_CLOCK_DOMAINS; d++) {
			if (soc[d] == 0 || soc[d] > 10000) continue;
			const char* name = d == 20 ? "gpu" : d == 23 ? "memory" : d == 24 ? "fabric" : "";
			buf_addf(b, "ps5_soc_clock_hertz{domain=\"%d\",name=\"%s\"} %llu\n", d, name, (unsigned long long)soc[d] * 1000000ULL);
			samples++;
		}
		if (samples == 0) buf_rewind(b, mark);
		else ok = 1;
	}
	int core[MAX_CORES * 2];
	for (int i = 0; i < MAX_CORES * 2; i++) core[i] = -1;
	int (*get_core)(int*) = OPTIONAL(int (*)(int*), sceKernelGetCpuCoreClock);
	if (get_core && get_core(core) == 0) {
		size_t mark = buf_mark(b);
		int samples = 0;
		buf_addf(b, "# HELP ps5_cpu_core_clock_hertz Clock per CPU core.\n# TYPE ps5_cpu_core_clock_hertz gauge\n");
		for (int c = 0; c < MAX_CORES; c++) {
			if (core[c] <= 0 || core[c] > 10000) continue;
			buf_addf(b, "ps5_cpu_core_clock_hertz{core=\"%d\"} %llu\n", c, (unsigned long long)core[c] * 1000000ULL);
			samples++;
		}
		if (samples == 0) buf_rewind(b, mark);
		else ok = 1;
	}
	metrics_note_collector("clocks", ok);
}

/* What the ICC says about the USB ports' and the disc drive's power (1 on,
   0 off on this console: USB 1, disc 0 without a disc drive attached). */
static void device_power(Buf* b)
{
	uint8_t usb = 0xff, bd = 0xff;
	int ok = 0;
	int (*get_usb)(uint8_t*) = OPTIONAL(int (*)(uint8_t*), sceKernelIccGetUSBPowerState);
	int (*get_bd)(uint8_t*) = OPTIONAL(int (*)(uint8_t*), sceKernelIccGetBDPowerState);
	if (get_usb && get_usb(&usb) == 0 && usb <= 1) {
		buf_addf(b, "# HELP ps5_usb_power_on Whether the USB ports have power.\n# TYPE ps5_usb_power_on gauge\nps5_usb_power_on %u\n", usb);
		ok = 1;
	}
	if (get_bd && get_bd(&bd) == 0 && bd <= 1) {
		buf_addf(b, "# HELP ps5_disc_drive_power_on Whether the disc drive has power.\n# TYPE ps5_disc_drive_power_on gauge\nps5_disc_drive_power_on %u\n", bd);
		ok = 1;
	}
	metrics_note_collector("device_power", ok);
}

static void lifetime(Buf* b)
{
	int ok = 0;
	uint64_t v = 0;
	int (*get_time)(uint64_t*) = OPTIONAL(int (*)(uint64_t*), sceKernelIccGetPowerOperatingTime);
	int (*get_cycles)(uint64_t*) = OPTIONAL(int (*)(uint64_t*), sceKernelIccGetPowerNumberOfBootShutdown);
	if (get_time && get_time(&v) == 0 && v > 0 && v < (uint64_t)100 * 365 * 24 * 3600) {
		buf_addf(b, "# HELP ps5_power_operating_seconds_total Time the console has been powered on since new.\n# TYPE ps5_power_operating_seconds_total counter\nps5_power_operating_seconds_total %llu\n", (unsigned long long)v);
		ok = 1;
	}
	v = 0;
	if (get_cycles && get_cycles(&v) == 0 && v > 0 && v < 10000000) {
		buf_addf(b, "# HELP ps5_power_cycles_total Number of power-on/off cycles since new.\n# TYPE ps5_power_cycles_total counter\nps5_power_cycles_total %llu\n", (unsigned long long)v);
		ok = 1;
	}
	metrics_note_collector("lifetime", ok);
}

void metrics_sensors(Buf* b)
{
	fan(b);
	power(b);
	clocks(b);
	device_power(b);
	lifetime(b);
}
