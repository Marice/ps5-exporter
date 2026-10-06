/* Host-test build of the parts of metrics.c that do not touch the console:
   the collector bookkeeping and the exporter's self-metrics. metrics.c
   itself needs libkernel, so it is not compiled on the host. */
#include "metrics.h"

#include <string.h>

#define MAX_COLLECTORS 12
static struct { const char* name; int ok; } g_collectors[MAX_COLLECTORS];
static int g_collector_count;

void metrics_note_collector(const char* name, int ok)
{
	for (int i = 0; i < g_collector_count; i++) {
		if (strcmp(g_collectors[i].name, name) == 0) {
			g_collectors[i].ok = ok;
			return;
		}
	}
	if (g_collector_count < MAX_COLLECTORS) {
		g_collectors[g_collector_count].name = name;
		g_collectors[g_collector_count].ok = ok;
		g_collector_count++;
	}
}

void metrics_self(Buf* b, unsigned long scrapes, const char* version)
{
	buf_addf(b, "# HELP ps5_exporter_build_info Exporter version.\n# TYPE ps5_exporter_build_info gauge\nps5_exporter_build_info{version=\"%s\"} 1\n", version);
	buf_addf(b, "# HELP ps5_exporter_scrapes_total Metrics requests served.\n# TYPE ps5_exporter_scrapes_total counter\nps5_exporter_scrapes_total %lu\n", scrapes);
	if (g_collector_count > 0) {
		buf_addf(b, "# HELP ps5_exporter_collector_success Whether a collector produced data during the last scrape.\n# TYPE ps5_exporter_collector_success gauge\n");
		for (int i = 0; i < g_collector_count; i++) {
			buf_addf(b, "ps5_exporter_collector_success{collector=\"%s\"} %d\n", g_collectors[i].name, g_collectors[i].ok);
		}
	}
}
