#ifndef POCKET_SHIKI_H
#define POCKET_SHIKI_H
#include "quickjs.h"
#include "esp_err.h"

/* pocket.shikiDebug: the Shiki v1 engine (main/ui/kasane/ksn_shiki.c) for the
 * on-device differential test (apps/shikitest, docs/kasane/shiki-spec.md
 * sec.10). Test-only, not the sec.9 API: app_session.c installs it only into
 * a session whose manifest names kasane.shiki.debug, as it does pocket.derby.
 * Holds no guest values and no memory between calls, so it has no reset. */
esp_err_t pocket_shiki_debug_install(JSContext *ctx, void *unused);
esp_err_t pocket_shiki_debug_fill(JSContext *ctx, JSValueConst ns, void *unused);
#endif
