#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"

// pocket.ui and pocket.input — section 6 of docs/common-api.md.
//
// This is the surface that replaces ui.setProp(id, 97, ...) for teaching. It
// does not remove the legacy globals: pocket.ui is built ON TOP of globalThis.ui
// rather than on the C core directly, and the two share one node tree.
//
// Going through the JS binding rather than pocketjs_ui_core_* is deliberate and
// is the single most important decision in this file:
//
//   * jsfont.c replaces ui.setText with a wrapper that scans every string for
//     characters the dynamic Japanese atlas does not hold yet. Calling the core
//     directly would skip that wrapper and every non-ASCII character would draw
//     as tofu. Calling ui.setText inherits it for free.
//   * ui.replaceText is NOT wrapped and is the same core call (Rust
//     replace_text delegates to set_text). That gives a second path with no
//     glyph bookkeeping at all, which is what the static ASCII atlases want:
//     text in font "small" or "large" now costs zero atlas rebuilds, where the
//     legacy ui.setText grew the Japanese slot by every letter it was shown.
//   * app_session.c builds the core after it installs the guest surfaces, so a
//     surface installed by pocketjs_guest_quickjs_install_once() has no core
//     pointer to hold anyway. globalThis.ui is looked up lazily, on the first
//     pocket.ui call, by which time the binding is mounted.
//
// Everything here runs on the JS owner task.

esp_err_t pocket_ui_install(JSContext *ctx, void *user_data);

// Wraps globalThis.ui's createNode and destroyNode so the node budget applies
// to apps on the legacy path too -- which is every app in apps/. Call from the
// JS task after the UI binding is mounted and before the app's source runs;
// without it the guard sees only nodes pocket.ui made itself, which is how a
// screen walked past the layout cliff and rebooted the device instead of being
// told no.
void pocket_ui_attach(JSContext *ctx);

// Turns the frame's button mask into ActionEvents and expires a toast. Call
// once per frame from the JS task with the same mask app_tick() was handed.
// Costs two loads and a branch when nothing is subscribed and no toast is up.
void pocket_ui_pump(uint32_t buttons);

// Drops every screen, node, list and input subscription. Call from the JS task
// while the guest is still alive — app_stop() before it destroys the guest —
// so the retained callbacks and item arrays are released into that realm.
void pocket_ui_reset(void);
