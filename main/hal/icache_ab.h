// I-cache auto-preload A/B probe (docs/vm/turn-icache.md sec.4). Diagnostic:
// compiled to nothing unless CONFIG_POCKET_ICACHE_AB.
#pragma once
#include "sdkconfig.h"

#ifdef CONFIG_POCKET_ICACHE_AB
// Before each frame() turn: puts the window's mode back (a flash write's
// cache re-enable restores the boot-time autoload bit).
void icache_ab_turn(void);
// After a KASANE_PAINT line: names the mode that window ran in, then moves
// the next window to the next mode.
void icache_ab_window(void);
#else
static inline void icache_ab_turn(void) {}
static inline void icache_ab_window(void) {}
#endif
