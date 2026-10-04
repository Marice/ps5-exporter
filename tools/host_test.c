/* Host test: serve /metrics with the ShadowMount collector only (the system
   collectors need PS5 libc), against a fake ShadowMount on 127.0.0.1:10101. */
#include <stdio.h>
#include <string.h>
#include "../src/http.h"
#include "../src/shadowmount.h"
static int handle(const char* path, char* body, size_t cap)
{
	if (strcmp(path, "/metrics") == 0) { size_t n = metrics_shadowmount(body, cap); body[n] = 0; return 200; }
	snprintf(body, cap, "not found\n"); return 404;
}
int main(void) { return http_serve(19100, handle, 0); }
