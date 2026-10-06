/* Minimal HTTP/1.0 server and client on BSD sockets. One request at a time
   is plenty for a Prometheus scrape every 15 seconds. */
#include "http.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* A whole request must arrive within this many milliseconds. The per-recv
   socket timeout alone does not bound the total: a client that dribbles one
   byte at a time keeps resetting it and would hold the single-threaded
   server forever. */
#define REQUEST_DEADLINE_MS 5000
#define RESPONSE_DEADLINE_MS 10000
#define SOCKET_TIMEOUT_MS 2000

static long long now_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void set_timeout(int fd, int ms)
{
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int send_all(int fd, const char* buf, size_t len, long long deadline)
{
	while (len > 0) {
		if (now_ms() > deadline) return -1;
		ssize_t n = send(fd, buf, len, 0);
		if (n <= 0) {
			if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
			return -1;
		}
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

static const char* status_text(int code)
{
	switch (code) {
	case 200: return "OK";
	case 404: return "Not Found";
	case 405: return "Method Not Allowed";
	default: return "Internal Server Error";
	}
}

/* Decide whether an accept() failure is transient or means the listening
   socket is gone (which happens when the console suspends and resumes).
   Returning 0 makes the caller rebind instead of spinning on a dead fd. */
static int accept_error_is_transient(int err)
{
	switch (err) {
	case EINTR:
	case ECONNABORTED:
	case EMFILE:
	case ENFILE:
	case ENOBUFS:
	case ENOMEM:
	case EAGAIN:
		return 1;
	default:
		return 0;
	}
}

int http_serve(int port, http_handler_t handler, volatile int* stop)
{
	if (port <= 0 || port > 65535) return -1;
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) return -1;
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);
	if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(srv, 8) < 0) {
		close(srv);
		return -1;
	}

	/* The metrics page is a few KB; 64 KB leaves room for many games.
	   Shared across requests: the loop handles one request at a time. */
	static char body[64 * 1024];
	static char header[256];
	static char req[8192];
	int consecutive_errors = 0;

	for (;;) {
		struct sockaddr_in peer;
		socklen_t peerlen = sizeof(peer);
		memset(&peer, 0, sizeof(peer));
		int fd = accept(srv, (struct sockaddr*)&peer, &peerlen);
		if (fd < 0) {
			int err = errno;
			if (!accept_error_is_transient(err) || ++consecutive_errors > 50) {
				/* The listener is unusable (suspend/resume, network down):
				   hand back so the caller can bind a fresh socket. */
				fprintf(stderr, "ps5-exporter: accept failed (errno %d), rebinding\n", err);
				close(srv);
				return -1;
			}
			usleep(100 * 1000);
			continue;
		}
		consecutive_errors = 0;
		set_timeout(fd, SOCKET_TIMEOUT_MS);
		long long read_deadline = now_ms() + REQUEST_DEADLINE_MS;
		size_t got = 0;
		int complete = 0;
		while (got < sizeof(req) - 1) {
			if (now_ms() > read_deadline) break;
			ssize_t n = recv(fd, req + got, sizeof(req) - 1 - got, 0);
			if (n <= 0) {
				if (n < 0 && errno == EINTR) continue;
				break;
			}
			got += (size_t)n;
			req[got] = 0;
			if (strstr(req, "\r\n\r\n") || strstr(req, "\n\n")) { complete = 1; break; }
		}
		if (!complete) {
			close(fd);
			continue;
		}
		char method[8] = { 0 }, path[512] = { 0 };
		if (sscanf(req, "%7s %511s", method, path) != 2) {
			close(fd);
			continue;
		}
		char* q = strchr(path, '?');
		if (q) *q = 0;

		int code;
		int is_get = strcmp(method, "GET") == 0;
		int is_head = strcmp(method, "HEAD") == 0;
		if (!is_get && !is_head) {
			code = 405;
			snprintf(body, sizeof(body), "method not allowed\n");
		} else {
			int from_loopback = (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
			code = handler(path, body, sizeof(body), from_loopback);
		}
		const char* ctype = (code == 200 && strcmp(path, "/metrics") == 0)
			? "text/plain; version=0.0.4; charset=utf-8"
			: "text/plain; charset=utf-8";
		size_t blen = strlen(body);
		int hlen = snprintf(header, sizeof(header),
		                    "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		                    code, status_text(code), ctype, blen);
		long long write_deadline = now_ms() + RESPONSE_DEADLINE_MS;
		if (hlen > 0 && send_all(fd, header, (size_t)hlen, write_deadline) == 0 && !is_head) {
			send_all(fd, body, blen, write_deadline);
		}
		close(fd);
		if (stop && *stop) {
			close(srv);
			return 0;
		}
	}
}

int http_post_json(const char* host, int port, const char* path, const char* json,
                   char* out, size_t out_cap, int timeout_ms)
{
	if (out_cap == 0) return -1;
	out[0] = 0;
	long long deadline = now_ms() + timeout_ms;
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	/* Bound each individual operation, and the whole exchange below. */
	set_timeout(fd, timeout_ms < SOCKET_TIMEOUT_MS ? timeout_ms : SOCKET_TIMEOUT_MS);
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons((uint16_t)port);
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1 || connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -2;
	}
	char req[1024];
	int rlen = snprintf(req, sizeof(req),
	                    "POST %s HTTP/1.0\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
	                    path, host, strlen(json), json);
	if (rlen <= 0 || (size_t)rlen >= sizeof(req) || send_all(fd, req, (size_t)rlen, deadline) < 0) {
		close(fd);
		return -3;
	}
	size_t got = 0;
	for (;;) {
		if (got + 1 >= out_cap) break;
		if (now_ms() > deadline) break;
		ssize_t n = recv(fd, out + got, out_cap - 1 - got, 0);
		if (n <= 0) {
			if (n < 0 && errno == EINTR) continue;
			break;
		}
		got += (size_t)n;
	}
	close(fd);
	out[got] = 0;
	int status = -4;
	if (got > 12 && strncmp(out, "HTTP/", 5) == 0) status = atoi(out + 9);
	/* Strip the headers so out holds only the body. */
	char* sep = strstr(out, "\r\n\r\n");
	if (sep) memmove(out, sep + 4, strlen(sep + 4) + 1);
	else out[0] = 0;
	return status;
}
