// G12 (docs/vm/backlog.md, L3/L4): does fragmentation make allocations fail
// on the device? Only compiled in with CONFIG_POCKET_VM_OOMPROBE -- see
// main/Kconfig.projbuild. Call sites include this unconditionally and wrap the
// calls in #ifdef, so the header makes the config macro visible itself.
#pragma once

#if defined(__has_include)
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif
#else
#include "sdkconfig.h"
#endif

#ifdef CONFIG_POCKET_VM_OOMPROBE

#include <stddef.h>
#include <stdint.h>

// Registers the heap's failed-allocation hook. Idempotent. Every heap_caps
// allocation that returns NULL -- the guest's, Kasane's, a JSON buffer's,
// Wi-Fi's -- lands in the hook, which is the only place that still sees the
// heap in the state that refused it.
void oomprobe_init(void);

// The runtime whose frame segments the hook should look for (a JSRuntime *;
// void here so this header stays free of quickjs.h), or NULL once it is gone.
// Recorded with the calling task, and the hook reads the segment chain only
// on that task: the chain belongs to whichever task runs the VM.
void oomprobe_set_runtime(void *rt);

// Prints and clears what the hook recorded ("G12 FAIL ..."). UI task only: the
// hook itself never prints, because it can run on any task and inside code
// that is in the middle of failing.
void oomprobe_drain(void);

// A rejection the guest's JS_TakeOOMCanary reported for the turn just ended.
// Most of them never reach the heap -- QuickJS's malloc_limit check refuses
// them first -- so the hook above does not see them; this line keeps them
// countable next to the heap failures.
void oomprobe_canary(uint32_t n, size_t first_req, size_t first_used);

// One line of totals ("G12 SESSION <label> ..."), then the counters restart.
// "home" at an app's start covers everything since the last app stopped;
// "app" at its stop covers the app.
void oomprobe_session_end(const char *label);

#endif // CONFIG_POCKET_VM_OOMPROBE
