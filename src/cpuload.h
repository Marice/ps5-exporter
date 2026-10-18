/* CPU load per logical CPU, from the idle threads' run time. */
#ifndef CPULOAD_H
#define CPULOAD_H

#include "buf.h"

/* Starts the sampling thread (every 2 seconds). */
void cpuload_start(void);

/* Appends ps5_cpu_usage_ratio per logical CPU once two samples exist. */
void metrics_cpuload(Buf* b);

#endif
