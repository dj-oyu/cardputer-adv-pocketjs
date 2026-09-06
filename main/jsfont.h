#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "quickjs.h"
#include "pocketjs/ui_core.h"

// Japanese text for JavaScript nodes.
//
// PocketJS keeps a font as one flat atlas of fixed cells, and the core copies
// whatever it is handed into its own heap. All of JIS X 0208 at 12x12 would be
// a megabyte of alpha bytes, which this board does not have, so the atlas holds
// only the characters a running program has actually shown: ui.setText is
// wrapped so every string it receives is scanned first, and the slot is rebuilt
// and reloaded whenever a new character turns up.
//
// Nodes select it with the font property: ui.setProp(id, 97, JSFONT_SLOT).
#define JSFONT_SLOT 2
#define JSFONT_MAX  160     // 160 x 152 B steady; the rebuild peak is ~3x that

// Called around a run. `attach` also clears the set.
void jsfont_attach(pocketjs_ui_core_t *core);
void jsfont_detach(void);

// Registers __pjs_glyphs. Pass to pocketjs_guest_quickjs_install_once().
esp_err_t jsfont_install(JSContext *ctx, void *user_data);

// The JS snippet that routes ui.setText through __pjs_glyphs. Evaluate it once
// the ui surface is mounted.
extern const char JSFONT_WRAP[];

unsigned jsfont_count(void);
