/* Metrics read from the ShadowMountPlus HTTP/JSON API on the console. */
#ifndef SHADOWMOUNT_H
#define SHADOWMOUNT_H

#include "buf.h"

/* Append ShadowMount metrics (availability, version, storage, games).
   When ShadowMount is not running only ps5_shadowmount_up 0 is written.
   All calls share one wall-clock budget; the game list is cached. */
void metrics_shadowmount(Buf* b);

/* Forget the cached game list. Only used by the tests, which need each
   case to start from a known state. */
void metrics_shadowmount_reset_cache(void);

#endif
