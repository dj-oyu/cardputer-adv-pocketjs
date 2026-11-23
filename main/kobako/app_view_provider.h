#ifndef KOBAKO_APP_VIEW_PROVIDER_H
#define KOBAKO_APP_VIEW_PROVIDER_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "quickjs.h"
#include "ui/kasane/ksn_view.h"

/* Application-owned view adapters are registered outside Kasane. The host
 * owns the APP lease, viewport and submission gate; providers borrow them. */
typedef struct {
    void *owner;
    ksn_view *view;
    ksn_rect viewport;
    const uint64_t *now_us;
    bool (*busy)(void *owner);
    void (*submitted)(void *owner,ksn_tx ticket,ksn_update_mode mode);
} kobako_app_view_host;

typedef struct {
    const char *name;
    JSValue (*mount)(JSContext *ctx,const kobako_app_view_host *host,void **out);
    /* Acknowledge a finished ticket without creating the next native frame. */
    ksn_result (*settle)(void *instance,bool *blocked);
    ksn_result (*step)(void *instance,bool *blocked);
    ksn_result (*host_status)(void *instance,const char *text,size_t bytes,
                              uint64_t until_us);
    size_t (*native_bytes)(const void *instance);
    void (*destroy)(void *instance);
} kobako_app_view_provider;

const kobako_app_view_provider *kobako_app_view_provider_lookup(const char *name);
#endif
