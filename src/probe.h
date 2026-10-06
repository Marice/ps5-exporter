/* Development-only probe endpoint, see probe.c. */
#ifndef PROBE_H
#define PROBE_H

#include "buf.h"

void probe_run(const char* what, Buf* b);

#endif
