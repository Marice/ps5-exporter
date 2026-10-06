/* ps5-exporter: a Prometheus exporter payload for a jailbroken PS5.
 *
 * Load it with the other payloads (Payload Manager autoload or elfldr). It
 * listens on port 9100 and answers GET /metrics with console temperatures,
 * uptime, memory, filesystems, network counters and, when ShadowMountPlus
 * runs, its storage and game list. Read-only; keep it on your own LAN.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "buf.h"
#include "http.h"
#include "metrics.h"
#include "sensors.h"
#include "shadowmount.h"
#ifdef EXPORTER_PROBE
#include "probe.h"
#endif

#ifndef EXPORTER_VERSION
#define EXPORTER_VERSION "0.0.0-dev"
#endif
#define EXPORTER_PORT 9100

int sceKernelSendNotificationRequest(uint32_t device, void* request, size_t size, int blocking);

struct NotificationRequest {
	uint8_t reserved[45];
	char message[3075];
};

static void notify(const char* text)
{
	static struct NotificationRequest req;
	memset(&req, 0, sizeof(req));
	snprintf(req.message, sizeof(req.message), "%s", text);
	(void)sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static unsigned long g_scrapes = 0;
static volatile int g_quit = 0;

static int handle(const char* path, char* body, size_t cap, int from_loopback)
{
	if (strcmp(path, "/metrics") == 0) {
		g_scrapes++;
		Buf b;
		buf_init(&b, body, cap);
		/* Collect first, then self-metrics, so collector results are known.
		   The buffer is written in two parts: metrics_self goes last but
		   must appear as its own families, which it does. */
		metrics_system(&b);
		metrics_sensors(&b);
		metrics_shadowmount(&b);
		metrics_self(&b, g_scrapes, EXPORTER_VERSION);
		if (b.truncated) {
			/* Partial output would be a misleading scrape: fail loudly so
			   Prometheus records an error instead of half the metrics. */
			snprintf(body, cap, "# metrics output did not fit in %zu bytes\n", cap);
			return 500;
		}
		return 200;
	}
	if (strcmp(path, "/health") == 0 || strcmp(path, "/healthz") == 0) {
		snprintf(body, cap, "ok\n");
		return 200;
	}
	if (strcmp(path, "/quit") == 0) {
		/* Stopping the exporter changes state, so keep it off the LAN: only
		   something running on the console itself may call it. */
		if (!from_loopback) {
			snprintf(body, cap, "not found\n");
			return 404;
		}
		snprintf(body, cap, "bye\n");
		g_quit = 1;
		return 200;
	}
#ifdef EXPORTER_PROBE
	if (strncmp(path, "/probe/", 7) == 0) {
		Buf b;
		buf_init(&b, body, cap);
		probe_run(path + 7, &b);
		return 200;
	}
#endif
	if (strcmp(path, "/") == 0) {
		snprintf(body, cap,
		         "ps5-exporter %s\n\nGET /metrics  Prometheus metrics\nGET /health   liveness check\n",
		         EXPORTER_VERSION);
		return 200;
	}
	snprintf(body, cap, "not found\n");
	return 404;
}

static int g_port = EXPORTER_PORT;

int main(int argc, char** argv)
{
	/* Optional port: first argument or EXPORTER_PORT in the environment
	   (handy to run a test build next to the autoloaded one). */
	const char* env_port = getenv("EXPORTER_PORT");
	if (env_port) {
		int p = atoi(env_port);
		if (p > 0 && p < 65536) g_port = p;
	}
	/* Loaders differ in whether argv[0] is the program or the first user
	   argument (websrv passes the args string as the whole argv), so any
	   purely numeric argument counts. */
	for (int i = 0; i < argc; i++) {
		int p = atoi(argv[i]);
		if (p > 0 && p < 65536 && strspn(argv[i], "0123456789") == strlen(argv[i])) g_port = p;
	}
	char msg[128];
	snprintf(msg, sizeof(msg), "ps5-exporter %s: metrics on port %d", EXPORTER_VERSION, g_port);
	fprintf(stderr, "%s\n", msg);
	notify(msg);
	/* Serve forever. http_serve also returns on a dead listening socket
	   (which is what suspend/resume can do), so binding a fresh one here is
	   the recovery path, not just a retry for a port still in TIME_WAIT. */
	int failures = 0;
	for (;;) {
		if (http_serve(g_port, handle, &g_quit) == 0) {
			fprintf(stderr, "ps5-exporter: quit requested\n");
			return 0;
		}
		failures++;
		fprintf(stderr, "ps5-exporter: listener on %d unavailable (attempt %d), retrying\n", g_port, failures);
		/* Tell the user once a minute of failures has passed: an invisible
		   dead exporter is worse than a notification. */
		if (failures == 12) {
			char warn[128];
			snprintf(warn, sizeof(warn), "ps5-exporter: cannot listen on port %d (already in use?)", g_port);
			notify(warn);
		}
		sleep(5);
	}
	return 0;
}
