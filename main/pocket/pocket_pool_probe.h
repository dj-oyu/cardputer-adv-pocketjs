#ifndef POCKET_POOL_PROBE_H
#define POCKET_POOL_PROBE_H
#include "quickjs.h"

#ifdef KASANE_P0_PROBE
/* Diagnostic-only real FreeRTOS producer for the generic Kasane source path. */
JSValue pocket_pool_probe_source(JSContext *,JSValueConst,int,JSValueConst *);
void pocket_pool_probe_reset(void);
#endif
#endif
