#ifndef POCKET_CLOCK_H
#define POCKET_CLOCK_H
#include "quickjs.h"
JSValue pocket_clock_wall(JSContext *,JSValueConst,int,JSValueConst *);
/* Service-owned native wall-clock source; fields 0=face, 1=tag. */
JSValue pocket_clock_wall_source(JSContext *,JSValueConst,int,JSValueConst *);
void pocket_clock_reset(void);
#endif
