/* Tiny HTTP/1.0 helpers: a blocking server loop and a JSON POST client. */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* Called for every request; must fill body (NUL-terminated) and return the
   HTTP status code. path is the request target without query string.
   from_loopback is non-zero when the peer is 127.0.0.0/8, so a handler can
   keep state-changing endpoints off the LAN. */
typedef int (*http_handler_t)(const char* path, char* body, size_t body_cap, int from_loopback);

/* Serve on 0.0.0.0:port until *stop becomes non-zero (checked after each
   request). Returns 0 then, or a negative value on a fatal socket error or
   when the listening socket becomes unusable (suspend/resume), so the caller
   can bind a fresh one. */
int http_serve(int port, http_handler_t handler, volatile int* stop);

/* POST json to http://host:port/path, store the response body (NUL-terminated)
   in out. Returns the HTTP status, or a negative value on connection errors.
   timeout_ms bounds the whole exchange, not just one socket operation. */
int http_post_json(const char* host, int port, const char* path, const char* json,
                   char* out, size_t out_cap, int timeout_ms);

#endif
