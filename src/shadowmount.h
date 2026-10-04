/* Metrics read from the ShadowMountPlus HTTP/JSON API on the console. */
#ifndef SHADOWMOUNT_H
#define SHADOWMOUNT_H

#include <stddef.h>

/* Append ShadowMount metrics (availability, version, storage, games).
   When ShadowMount is not running only ps5_shadowmount_up 0 is written. */
size_t metrics_shadowmount(char* out, size_t cap);

#endif
