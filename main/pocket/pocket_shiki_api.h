#ifndef POCKET_SHIKI_API_H
#define POCKET_SHIKI_API_H
/* pocket.kasane.shiki (docs/kasane/shiki-spec.md sec.9), pocket_shiki_api.c. */
#include "esp_err.h"
#include "quickjs.h"

/* The kasane.shiki capability, for every session (supported; available when
 * the session has a Shiki heap). */
esp_err_t pocket_shiki_api_register(void);
/* kasane.shiki into the pocket.kasane namespace, after its procedural and
 * grid members: only for a session whose manifest names kasane.shiki. */
esp_err_t pocket_shiki_api_install(JSContext *ctx,JSValueConst kasane);
/* app_stop(), before the guest is destroyed: every window detached, Shiki's
 * references dropped, the regions' memory back in the Shiki heap. */
void pocket_shiki_api_reset(void);
#endif
