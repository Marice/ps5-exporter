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

#include "http.h"
#include "metrics.h"
#include "shadowmount.h"
#ifdef EXPORTER_PROBE
#include "probe.h"
#endif

#define EXPORTER_VERSION "0.1.1"
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
	if (strcmp(path, "/quit") == 0) {
		/* Lets a newer build take over the port without a console reboot.
		   Read-only otherwise, and LAN only, so this is acceptable. */
		snprintf(body, cap, "bye\n");
		g_quit = 1;
		return 200;
	}
#ifdef EXPORTER_PROBE
	if (strncmp(path, "/probe/", 7) == 0) {
		size_t n = probe_run(path + 7, body, cap);
		body[n < cap ? n : cap - 1] = 0;
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
	if (env_port && atoi(env_port) > 0) g_port = atoi(env_port);
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
	/* Serve forever; retry the bind if the port is briefly still in use
	   after a previous instance (payload reloads). */
	for (;;) {
		if (http_serve(g_port, handle, &g_quit) == 0) {
			fprintf(stderr, "ps5-exporter: quit requested\n");
			return 0;
		} else {
			fprintf(stderr, "ps5-exporter: bind/listen on %d failed, retrying\n", g_port);
			sleep(5);
		}
	}
	return 0;
}
