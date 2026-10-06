/* Hardware sensors beyond the temperatures: fan, per-core CPU usage, SoC
 * power and lifetime counters.
 *
 * These libkernel functions are not in the SDK headers. The signatures come
 * from two public homebrew projects that call them on real consoles:
 *   - sceKernelGetCurrentFanDuty(uint16_t* duty, uint64_t* chassis), duty on
 *     a 0..1024 scale: drakmor/fan_target (the ShadowMountPlus author).
 *   - sceKernelGetCpuUsageAll(int* per_core_pct, int* count) and
 *     sceKernelGetSocPowerConsumption(uint64_t* out, double reserved):
 *     aloksaurabh/elf-arsenal.
 * Every value is range-checked before it becomes a metric, so a console that
 * refuses a call or returns something unexpected simply omits that metric
 * instead of publishing nonsense. */
#include "sensors.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "metrics.h"

int sceKernelGetCurrentFanDuty(uint16_t* duty, uint64_t* chassis);
int sceKernelGetCpuUsageAll(int* per_core_pct, int* count_out);
int sceKernelGetSocPowerConsumption(uint64_t* out, double reserved);
int sceKernelIccGetPowerNumberOfBootShutdown(uint64_t* out);
int sceKernelIccGetPowerOperatingTime(uint64_t* out);
int sceKernelGetBasicProductShape(int* out);

#define FAN_DUTY_SCALE 1024.0
/* The kernel fills this array without knowing its size, so keep generous
   headroom: the console has 8 cores, we pass 32 slots. */
#define CORE_SLOTS 32
#define MAX_CORES 16

static void fan(Buf* b)
{
	uint16_t duty = 0xffff;
	uint64_t chassis = 0;
	if (sceKernelGetCurrentFanDuty(&duty, &chassis) != 0 || duty > FAN_DUTY_SCALE) {
		metrics_note_collector("fan", 0);
		return;
	}
	/* One canonical representation: a 0..1 ratio, the Prometheus convention
	   for a bounded fraction. Grafana renders it as a percentage. */
	buf_addf(b, "# HELP ps5_fan_duty_ratio Fan duty cycle, 0 to 1 (kernel reports 0..1024).\n# TYPE ps5_fan_duty_ratio gauge\nps5_fan_duty_ratio %.4f\n", duty / FAN_DUTY_SCALE);
	metrics_note_collector("fan", 1);
}

static void cpu_usage(Buf* b)
{
	int per_core[CORE_SLOTS];
	int count = 0;
	for (int i = 0; i < CORE_SLOTS; i++) per_core[i] = -1;
	if (sceKernelGetCpuUsageAll(per_core, &count) != 0 || count <= 0 || count > MAX_CORES) {
		metrics_note_collector("cpu_usage", 0);
		return;
	}
	size_t mark = buf_mark(b);
	int valid = 0;
	/* Per core only: no aggregate inside the same family, so sum() and
	   avg() over this metric stay correct. Use avg() for the overall load. */
	buf_addf(b, "# HELP ps5_cpu_usage_ratio CPU usage per core, 0 to 1. Use avg() for the overall load.\n# TYPE ps5_cpu_usage_ratio gauge\n");
	for (int i = 0; i < count; i++) {
		if (per_core[i] < 0 || per_core[i] > 100) continue;
		buf_addf(b, "ps5_cpu_usage_ratio{core=\"%d\"} %.2f\n", i, per_core[i] / 100.0);
		valid++;
	}
	if (valid == 0) {
		buf_rewind(b, mark);
		metrics_note_collector("cpu_usage", 0);
		return;
	}
	buf_addf(b, "# HELP ps5_cpu_cores Number of CPU cores the kernel reports usage for.\n# TYPE ps5_cpu_cores gauge\nps5_cpu_cores %d\n", count);
	metrics_note_collector("cpu_usage", 1);
}

static void soc_power(Buf* b)
{
	uint64_t raw[16];
	memset(raw, 0, sizeof(raw));
	if (sceKernelGetSocPowerConsumption(raw, 0.0) != 0) {
		metrics_note_collector("soc_power", 0);
		return;
	}
	/* The unit is not documented; community findings point at milliwatts.
	   Only publish watts when the value is plausible for a console, and
	   keep the raw reading under its own name so an unexpected unit can be
	   spotted instead of silently producing a blank panel. */
	buf_addf(b, "# HELP ps5_soc_power_raw_value Raw first word of sceKernelGetSocPowerConsumption (unit unconfirmed).\n# TYPE ps5_soc_power_raw_value gauge\nps5_soc_power_raw_value %llu\n", (unsigned long long)raw[0]);
	int plausible = raw[0] >= 1000 && raw[0] <= 350000;
	if (plausible) {
		buf_addf(b, "# HELP ps5_soc_power_watts SoC power, assuming the raw value is milliwatts.\n# TYPE ps5_soc_power_watts gauge\nps5_soc_power_watts %.2f\n", raw[0] / 1000.0);
	}
	metrics_note_collector("soc_power", plausible);
}

static void lifetime(Buf* b)
{
	int ok = 0;
	uint64_t v = 0;
	if (sceKernelIccGetPowerOperatingTime(&v) == 0 && v > 0 && v < (uint64_t)100 * 365 * 24 * 3600) {
		buf_addf(b, "# HELP ps5_power_operating_seconds_total Time the console has been powered on since new.\n# TYPE ps5_power_operating_seconds_total counter\nps5_power_operating_seconds_total %llu\n", (unsigned long long)v);
		ok = 1;
	}
	v = 0;
	if (sceKernelIccGetPowerNumberOfBootShutdown(&v) == 0 && v > 0 && v < 10000000) {
		buf_addf(b, "# HELP ps5_power_cycles_total Number of power-on/off cycles since new.\n# TYPE ps5_power_cycles_total counter\nps5_power_cycles_total %llu\n", (unsigned long long)v);
		ok = 1;
	}
	metrics_note_collector("lifetime", ok);
}

void metrics_sensors(Buf* b)
{
	fan(b);
	cpu_usage(b);
	soc_power(b);
	lifetime(b);
}
