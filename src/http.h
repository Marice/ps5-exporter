/* Tiny HTTP/1.0 helpers: a blocking server loop and a JSON POST client. */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* Called for every request; must fill body (NUL-terminated) and return the
   HTTP status code. path is the request target without query string. */
typedef int (*http_handler_t)(const char* path, char* body, size_t body_cap);

/* Serve on 0.0.0.0:port until *stop becomes non-zero (checked after each
   request). Returns 0 then, or a negative value on a fatal socket error. */
int http_serve(int port, http_handler_t handler, volatile int* stop);

/* POST json to http://host:port/path, store the response body (NUL-terminated)
   in out. Returns the HTTP status, or a negative value on connection errors. */
int http_post_json(const char* host, int port, const char* path, const char* json,
                   char* out, size_t out_cap, int timeout_ms);

#endif
