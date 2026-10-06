/* Fan, per-core CPU usage, SoC power and lifetime counters. */
#ifndef SENSORS_H
#define SENSORS_H

#include "buf.h"

/* Append the hardware sensor metrics. Calls the console refuses, or values
   outside a plausible range, are skipped silently. */
void metrics_sensors(Buf* b);

#endif
