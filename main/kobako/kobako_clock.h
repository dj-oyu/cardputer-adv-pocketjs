#ifndef KOBAKO_CLOCK_H
#define KOBAKO_CLOCK_H
#include "quickjs.h"
JSValue kobako_clock_wall(JSContext *,JSValueConst,int,JSValueConst *);
/* Service-owned native wall-clock source; fields 0=face, 1=tag. */
JSValue kobako_clock_wall_source(JSContext *,JSValueConst,int,JSValueConst *);
void kobako_clock_reset(void);
#endif
