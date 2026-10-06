/* System metrics collected on the console itself. */
#ifndef METRICS_H
#define METRICS_H

#include "buf.h"

/* Appends system metrics: console info, temperatures, CPU frequency, uptime,
   direct memory, filesystems, network counters and the process table. */
void metrics_system(Buf* b);

/* Exporter self-metrics: build info, scrape counter, per-collector success. */
void metrics_self(Buf* b, unsigned long scrapes, const char* version);

/* Records whether a collector produced anything, exposed as
   ps5_exporter_collector_success so a silently empty family is visible. */
void metrics_note_collector(const char* name, int ok);

#endif
