#pragma once
#include "app_chunks.h"

// The pocket.app.load() chunk table on a host, filled from the same chunks.txt
// the firmware build reads (tools/make_app_chunks.py has the format). Link this
// file instead of the generated app_chunks.c, together with
// main/pocket/pocket_app_load.c and main/pocket/app_registry.c, and a harness
// that evaluates an app on the real QuickJS can run one that calls
// pocket.app.load(). docs/vm/eval-peak.md section 7.4 has the recipe.
//
// Not thread-safe and not meant to be: a harness reads its lists once, before
// the first guest, and clears them at exit (ASan counts what is left).

// Reads one chunks.txt and the files it names (relative to the list), each into
// its own NUL-terminated buffer. Lists naming the same app are merged. Returns
// 0, or -1 after printing why to stderr -- with the same refusals as the build:
// a bad or repeated name, a 33rd chunk, a missing file.
int app_chunks_host_read(const char *list_path);

// Frees every buffer and forgets every set.
void app_chunks_host_clear(void);
