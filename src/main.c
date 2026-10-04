/* ps5-exporter: a Prometheus exporter payload for a jailbroken PS5.
 *
 * Load it with the other payloads (Payload Manager autoload or elfldr). It
 * listens on port 9100 and answers GET /metrics with console temperatures,
 * uptime, memory, filesystems, network counters and, when ShadowMountPlus
 * runs, its storage and game list. Read-only; keep it on your own LAN.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "http.h"
#include "metrics.h"
#include "shadowmount.h"

#define EXPORTER_VERSION "0.1.0"
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

static int handle(const char* path, char* body, size_t cap)
{
	if (strcmp(path, "/metrics") == 0) {
		g_scrapes++;
		size_t len = 0;
		len += metrics_self(body + len, cap - len, g_scrapes, EXPORTER_VERSION);
		len += metrics_system(body + len, cap - len);
		len += metrics_shadowmount(body + len, cap - len);
		if (len >= cap - 1) {
			/* Truncated output would be invalid for Prometheus; say so. */
			snprintf(body, cap, "# metrics output exceeded %zu bytes\n", cap);
			return 500;
		}
		return 200;
	}
	if (strcmp(path, "/health") == 0 || strcmp(path, "/healthz") == 0) {
		snprintf(body, cap, "ok\n");
		return 200;
	}
	if (strcmp(path, "/") == 0) {
		snprintf(body, cap,
		         "ps5-exporter %s\n\nGET /metrics  Prometheus metrics\nGET /health   liveness check\n",
		         EXPORTER_VERSION);
		return 200;
	}
	snprintf(body, cap, "not found\n");
	return 404;
}

int main(void)
{
	char msg[128];
	snprintf(msg, sizeof(msg), "ps5-exporter %s: metrics on port %d", EXPORTER_VERSION, EXPORTER_PORT);
	fprintf(stderr, "%s\n", msg);
	notify(msg);
	/* Serve forever; retry the bind if the port is briefly still in use
	   after a previous instance (payload reloads). */
	for (;;) {
		if (http_serve(EXPORTER_PORT, handle) < 0) {
			fprintf(stderr, "ps5-exporter: bind/listen on %d failed, retrying\n", EXPORTER_PORT);
			sleep(5);
		}
	}
	return 0;
}
