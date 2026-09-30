#pragma once
#include <stddef.h>
#include <stdint.h>

// The chunks an app may evaluate with pocket.app.load(name) (docs/api/
// common-api.md section 5, docs/vm/eval-peak.md section 7).
//
// Why an app is cut into chunks at all: QuickJS keeps every JSFunctionDef of a
// script alive until the whole script has been parsed (js_create_function), so
// a source's parse peak is one number for the whole file. Evaluated as several
// global scripts in one realm it is paid once per script, over what the
// earlier ones left -- measured on the board, DERBY WATCH's evaluation headroom
// went from 5.0 KB to 23.6 KB cut in three.
//
// The table is data the build writes. tools/make_app_chunks.py reads every
// apps/*/chunks.txt, embeds the files it names and generates app_chunks.c with
// one set per app id; the host harnesses fill the same table from the same
// chunks.txt through tools/hostshim/app_chunks_host.c. Nothing here depends on
// ESP-IDF or QuickJS, so both sides link the one header.

// A set's state in pocket_app_load.c is a few 32-bit masks, one bit a chunk,
// so a set holds at most 32; make_app_chunks.py refuses a 33rd at build time
// and app_chunks_host.c at load, so the limit is never met at run time.
#define APP_CHUNKS_MAX      32
// A name is what the app passes to load(); short, because it is typed into
// JS source that is itself parsed on a 160 KiB heap.
#define APP_CHUNK_NAME_MAX  31

typedef struct {
    const char *name;    // what pocket.app.load() is called with
    const char *file;    // the file's base name: what an error's stack shows
    // The bytes, and one past the NUL that ends them: the pair the embedded
    // file's _start/_end symbols are. A length would be neater, but the
    // difference of two link-time symbols is not a constant a static table
    // can hold, and one written down at configure time goes stale the first
    // time the file is edited without a reconfigure. JS_Eval also wants that
    // NUL at start[length], and both producers of this table leave it there.
    const char *start;
    const char *end;
    // 1 for an ES module (the file ends in .mjs): reached only by a static
    // `import` from the app's module entry, never by load(). 0 for a global
    // script, reached only by load(). The two are kept apart because a chunk
    // evaluated both ways would be two unrelated copies in one realm, and a
    // module stays resident for the realm's life (ctx->loaded_modules) where
    // a script's top level is freed after it runs. The kind comes from the
    // file name, the convention every other JS host uses, so chunks.txt
    // needs no column for it.
    uint8_t module;
} app_chunk_t;

typedef struct {
    const char        *app_id;   // an app_registry.c id, "local.derby"
    const app_chunk_t *chunks;
    uint32_t           count;
} app_chunk_set_t;

// The set for one app id, or NULL when that app has no chunks -- which is
// every app that receives its source at run time (Playground, Tutorial, a
// work), and they get NOT_FOUND from load(). Defined by the generated
// app_chunks.c in the firmware and by tools/hostshim/app_chunks_host.c on a
// host.
const app_chunk_set_t *app_chunks_for(const char *app_id);
