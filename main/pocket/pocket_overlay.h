#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"
#include "overlay_core.h"

// pocket.overlay — the drawing surface of docs/common-api.md 3.1.
//
// WHY THIS IS NOT ui.basic, which is what 3.1 names.
//
// ui.basic is pocket_ui.c, which is built on globalThis.ui, which is the Rust
// UI core and the rgb565 renderer. Two properties of that path make it the
// wrong one for something that lives ON TOP of the home screen's background:
//
//   * render_strip fills its region with 0 before it draws anything
//     (engine/backends/rgb565/src/lib.rs, the fill_rgb565(.., 0) before
//     render_region). An overlay rendered through it would not sit over the
//     scene; it would punch an opaque black hole in it.
//   * the font atlas rebuild that ui.setText triggers is, by 3.1's own
//     account, the largest single allocation a small app makes -- and an
//     overlay wants it at the moment the scene's scratch is also held.
//
// So the overlay draws through a host-owned display list instead: the guest
// appends a few dozen boxes and strings, and ui/overlay.c composites them into
// the shell's strip under the shell's own labels. That also makes 3.1's
// "はみ出しは切り取るのではなく拒否する" a property of the surface rather than
// an intention -- every call is checked against the region and throws when it
// does not fit, and the coordinates are region-local so the shell's own rows
// are not addressable at all.
//
// Everything here runs on the JS owner task.

esp_err_t pocket_overlay_install(JSContext *ctx, void *user_data);

// The box the session may draw in. Set before the guest's source runs; the
// limits an app reads are taken from it.
void pocket_overlay_set_region(const overlay_region_t *region);

// Composites the current display list into one strip of the shell's frame.
// Cheap and silent when nothing is published.
void pocket_overlay_paint(uint16_t *strip, int strip_y, int strip_h);

// True when the guest published anything this session.
bool pocket_overlay_drawn(void);

// Drops the display list and the region. Called from app_stop() before the
// guest dies, with every other surface.
void pocket_overlay_reset(void);
