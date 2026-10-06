/* Fan, per-core CPU usage, SoC power and lifetime counters. */
#ifndef SENSORS_H
#define SENSORS_H

#include <stddef.h>

/* Append the hardware sensor metrics. Calls the console refuses, or values
   outside a plausible range, are skipped silently. */
size_t metrics_sensors(char* out, size_t cap);

#endif
