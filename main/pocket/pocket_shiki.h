#ifndef POCKET_SHIKI_H
#define POCKET_SHIKI_H
#include "quickjs.h"
#include "esp_err.h"

/* pocket.shikiDebug: the Shiki v1 engine (main/ui/kasane/ksn_shiki.c) for the
 * on-device differential test (apps/shikitest, docs/kasane/shiki-spec.md
 * sec.10). Test-only, not the sec.9 API: app_session.c installs it only into
 * a session whose manifest names kasane.shiki.debug, as it does pocket.derby.
 * Diagnostic incremental preflight owns no guest values; teardown releases its workspace. */
esp_err_t pocket_shiki_debug_install(JSContext *ctx, void *unused);
esp_err_t pocket_shiki_debug_fill(JSContext *ctx, JSValueConst ns, void *unused);
#ifdef KSN_MESH_Q7_BENCH_PREFLIGHT
void pocket_shiki_debug_reset(void);
#endif
#endif
