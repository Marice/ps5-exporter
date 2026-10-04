/* Tiny HTTP/1.0 helpers: a blocking server loop and a JSON POST client. */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* Called for every request; must fill body (NUL-terminated) and return the
   HTTP status code. path is the request target without query string. */
typedef int (*http_handler_t)(const char* path, char* body, size_t body_cap);

/* Serve forever on 0.0.0.0:port. Returns only on a fatal socket error. */
int http_serve(int port, http_handler_t handler);

/* POST json to http://host:port/path, store the response body (NUL-terminated)
   in out. Returns the HTTP status, or a negative value on connection errors. */
int http_post_json(const char* host, int port, const char* path, const char* json,
                   char* out, size_t out_cap, int timeout_ms);

#endif
