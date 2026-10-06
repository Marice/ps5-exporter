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

#define APPEND(...) do { int n_ = snprintf(out + len, cap > len ? cap - len : 0, __VA_ARGS__); if (n_ > 0) len += (size_t)n_; } while (0)

static size_t fan(char* out, size_t cap)
{
	size_t len = 0;
	uint16_t duty = 0xffff;
	uint64_t chassis = 0;
	if (sceKernelGetCurrentFanDuty(&duty, &chassis) != 0) return 0;
	if (duty > FAN_DUTY_SCALE) return 0;
	APPEND("# HELP ps5_fan_duty_percent Fan duty cycle (raw value scaled from 0..1024).\n# TYPE ps5_fan_duty_percent gauge\nps5_fan_duty_percent %.1f\n", duty * 100.0 / FAN_DUTY_SCALE);
	APPEND("# HELP ps5_fan_duty_raw Fan duty cycle as the kernel reports it (0..1024).\n# TYPE ps5_fan_duty_raw gauge\nps5_fan_duty_raw %u\n", (unsigned)duty);
	return len;
}

static size_t cpu_usage(char* out, size_t cap)
{
	size_t len = 0;
	int per_core[CORE_SLOTS];
	int count = 0;
	for (int i = 0; i < CORE_SLOTS; i++) per_core[i] = -1;
	if (sceKernelGetCpuUsageAll(per_core, &count) != 0) return 0;
	if (count <= 0 || count > MAX_CORES) return 0;
	long total = 0;
	int valid = 0;
	APPEND("# HELP ps5_cpu_usage_percent CPU usage per core, and the average over all cores.\n# TYPE ps5_cpu_usage_percent gauge\n");
	for (int i = 0; i < count; i++) {
		if (per_core[i] < 0 || per_core[i] > 100) continue;
		APPEND("ps5_cpu_usage_percent{core=\"%d\"} %d\n", i, per_core[i]);
		total += per_core[i];
		valid++;
	}
	if (valid > 0) APPEND("ps5_cpu_usage_percent{core=\"all\"} %.1f\n", (double)total / valid);
	APPEND("# HELP ps5_cpu_cores Number of CPU cores the kernel reports usage for.\n# TYPE ps5_cpu_cores gauge\nps5_cpu_cores %d\n", count);
	return len;
}

static size_t soc_power(char* out, size_t cap)
{
	size_t len = 0;
	uint64_t buf[16];
	memset(buf, 0, sizeof(buf));
	if (sceKernelGetSocPowerConsumption(buf, 0.0) != 0) return 0;
	/* The unit is not documented; community findings point at milliwatts.
	   Publish the raw value and a watts reading only when it is plausible
	   for a console (1 W to 350 W). */
	APPEND("# HELP ps5_soc_power_raw Raw first word of sceKernelGetSocPowerConsumption (unit unconfirmed).\n# TYPE ps5_soc_power_raw gauge\nps5_soc_power_raw %llu\n", (unsigned long long)buf[0]);
	if (buf[0] >= 1000 && buf[0] <= 350000) {
		APPEND("# HELP ps5_soc_power_watts SoC power, assuming the raw value is milliwatts.\n# TYPE ps5_soc_power_watts gauge\nps5_soc_power_watts %.2f\n", buf[0] / 1000.0);
	}
	return len;
}

static size_t lifetime(char* out, size_t cap)
{
	size_t len = 0;
	uint64_t v = 0;
	if (sceKernelIccGetPowerOperatingTime(&v) == 0 && v > 0 && v < (uint64_t)100 * 365 * 24 * 3600) {
		APPEND("# HELP ps5_power_operating_seconds_total Time the console has been powered on since new.\n# TYPE ps5_power_operating_seconds_total counter\nps5_power_operating_seconds_total %llu\n", (unsigned long long)v);
	}
	v = 0;
	if (sceKernelIccGetPowerNumberOfBootShutdown(&v) == 0 && v > 0 && v < 10000000) {
		APPEND("# HELP ps5_power_cycles_total Number of power-on/off cycles since new.\n# TYPE ps5_power_cycles_total counter\nps5_power_cycles_total %llu\n", (unsigned long long)v);
	}
	return len;
}

size_t metrics_sensors(char* out, size_t cap)
{
	size_t len = 0;
	len += fan(out + len, cap - len);
	len += cpu_usage(out + len, cap - len);
	len += soc_power(out + len, cap - len);
	len += lifetime(out + len, cap - len);
	return len;
}
