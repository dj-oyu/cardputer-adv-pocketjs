#ifndef KOBAKO_APP_VIEW_ASSETS_H
#define KOBAKO_APP_VIEW_ASSETS_H
#include "ui/kasane/ksn_schema.h"
#include "ui/kasane/ksn_source.h"
#include "ui/kasane/ksn_ports.h"
typedef struct {
    size_t source_bytes;
    ksn_result (*source_open)(void *storage,ksn_source_provider *out);
    void (*source_registered)(void *storage,ksn_source_handle handle);
    const ksn_source_binding *source_bindings;
    uint8_t source_binding_count;
} kobako_app_view_source;
typedef struct {
    const ksn_schema *schema;
    const kobako_app_view_source *sources;
    uint8_t source_count;
} kobako_app_view_asset;
/* Package-owned immutable assets and optional domain producers. Kasane sees
 * only typed values and does not recognize application identities. */
const kobako_app_view_asset *kobako_app_view_lookup(const char *name);
#ifdef KSN_TEST_DUAL_SOURCE
void kobako_test_dual_set(unsigned source,const char *text);
void kobako_test_dual_expire(unsigned source,uint64_t at_us);
void kobako_test_dual_deny(unsigned source,bool deny);
void kobako_test_dual_fail_second(bool fail);
void kobako_test_dual_counts(unsigned *acquired,unsigned *released);
#endif
typedef struct {
    const char *name;
    uint16_t width,height,variants,frames;
    ksn_result (*open)(ksn_image_port *out);
} kobako_app_image_asset;
const kobako_app_image_asset *kobako_app_image_lookup(const char *name);
#endif
