/* System metrics collected on the console itself. */
#ifndef METRICS_H
#define METRICS_H

#include <stddef.h>

/* Append system metrics (temperatures, uptime, memory, filesystems,
   network counters, process count) in Prometheus text format. Returns the
   number of characters written. */
size_t metrics_system(char* out, size_t cap);

/* Exporter self-metrics (scrape counter, build info). */
size_t metrics_self(char* out, size_t cap, unsigned long scrapes, const char* version);

#endif
