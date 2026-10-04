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
#include <unistd.h>

static void set_timeout(int fd, int ms)
{
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int send_all(int fd, const char* buf, size_t len)
{
	while (len > 0) {
		ssize_t n = send(fd, buf, len, 0);
		if (n <= 0) return -1;
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

int http_serve(int port, http_handler_t handler)
{
	int srv = socket(AF_INET, SOCK_STREAM, 0);
	if (srv < 0) return -1;
	int one = 1;
	setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons((uint16_t)port);
	if (bind(srv, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(srv, 4) < 0) {
		close(srv);
		return -1;
	}

	/* The metrics page is a few KB; 64 KB leaves room for many games. */
	static char body[64 * 1024];
	static char header[256];
	static char req[4096];

	for (;;) {
		int fd = accept(srv, NULL, NULL);
		if (fd < 0) {
			if (errno == EINTR) continue;
			usleep(100 * 1000);
			continue;
		}
		set_timeout(fd, 3000);
		size_t got = 0;
		int complete = 0;
		while (got < sizeof(req) - 1) {
			ssize_t n = recv(fd, req + got, sizeof(req) - 1 - got, 0);
			if (n <= 0) break;
			got += (size_t)n;
			req[got] = 0;
			if (strstr(req, "\r\n\r\n")) { complete = 1; break; }
		}
		if (!complete) {
			close(fd);
			continue;
		}
		char method[8] = { 0 }, path[512] = { 0 };
		sscanf(req, "%7s %511s", method, path);
		char* q = strchr(path, '?');
		if (q) *q = 0;

		int code;
		if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0) {
			code = 405;
			snprintf(body, sizeof(body), "method not allowed\n");
		} else {
			code = handler(path, body, sizeof(body));
		}
		const char* ctype = (code == 200 && strcmp(path, "/metrics") == 0)
			? "text/plain; version=0.0.4; charset=utf-8"
			: "text/plain; charset=utf-8";
		size_t blen = strlen(body);
		int hlen = snprintf(header, sizeof(header),
		                    "HTTP/1.0 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
		                    code, status_text(code), ctype, blen);
		if (send_all(fd, header, (size_t)hlen) == 0 && strcmp(method, "HEAD") != 0) send_all(fd, body, blen);
		close(fd);
	}
}

int http_post_json(const char* host, int port, const char* path, const char* json,
                   char* out, size_t out_cap, int timeout_ms)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return -1;
	set_timeout(fd, timeout_ms);
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
	if (send_all(fd, req, (size_t)rlen) < 0) {
		close(fd);
		return -3;
	}
	size_t got = 0;
	for (;;) {
		if (got + 1 >= out_cap) break;
		ssize_t n = recv(fd, out + got, out_cap - 1 - got, 0);
		if (n <= 0) break;
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
