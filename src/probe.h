/* Development-only probe endpoint, see probe.c. */
#ifndef PROBE_H
#define PROBE_H
#include <stddef.h>
size_t probe_run(const char* what, char* out, size_t cap);
#endif
