#pragma once

#include "kobako/guest.h"
#include "quickjs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Version-pinned escape hatch used by native surface components. */
typedef esp_err_t (*kobako_guest_quickjs_install_fn)(JSContext *context,
                                                       void *user_data);

esp_err_t
kobako_guest_quickjs_install(kobako_guest_t *guest,
                               kobako_guest_quickjs_install_fn install,
                               void *user_data);

/** Install a named surface once per realm. Names are copied. A failed
 * installer releases its reservation. Neither context nor runtime opaque
 * slots are used by this registry. */
esp_err_t
kobako_guest_quickjs_install_once(kobako_guest_t *guest, const char *name,
                                    kobako_guest_quickjs_install_fn install,
                                    void *user_data);

/** Valid only for the duration of a synchronous owner-task operation. */
JSContext *kobako_guest_quickjs_context(kobako_guest_t *guest);

#ifdef __cplusplus
}
#endif
