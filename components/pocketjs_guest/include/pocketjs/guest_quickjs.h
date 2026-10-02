#pragma once

#include "pocketjs/guest.h"
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Version-pinned escape hatch used by native surface components. */
typedef esp_err_t (*pocketjs_guest_quickjs_install_fn)(JSContext *context,
                                                       void *user_data);

esp_err_t
pocketjs_guest_quickjs_install(pocketjs_guest_t *guest,
                               pocketjs_guest_quickjs_install_fn install,
                               void *user_data);

/** Install a named surface once per realm. Names are copied. A failed
 * installer releases its reservation. Neither context nor runtime opaque
 * slots are used by this registry. */
esp_err_t
pocketjs_guest_quickjs_install_once(pocketjs_guest_t *guest, const char *name,
                                    pocketjs_guest_quickjs_install_fn install,
                                    void *user_data);

/** Valid only for the duration of a synchronous owner-task operation. */
JSContext *pocketjs_guest_quickjs_context(pocketjs_guest_t *guest);

/** Report a completed frame's exception, including a resumed frame. The value
 * is borrowed and the original exception is preserved for the stderr dump.
 * Called only after the VM has closed its chain, never while parked. */
typedef void (*pocketjs_guest_frame_error_fn)(JSContext *, JSValueConst, void *);
void pocketjs_guest_set_frame_error_handler(pocketjs_guest_t *guest,
                                            pocketjs_guest_frame_error_fn fn,
                                            void *opaque);

#ifdef __cplusplus
}
#endif
