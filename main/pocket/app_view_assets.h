#ifndef POCKET_APP_VIEW_ASSETS_H
#define POCKET_APP_VIEW_ASSETS_H
#include "ui/kasane/ksn_schema.h"
#include "ui/kasane/ksn_ports.h"
typedef struct {
    const ksn_schema *schema;
    size_t source_bytes;
    ksn_result (*source_read)(void *source,ksn_schema_value *values,uint64_t *revision);
} pocket_app_view_asset;
/* Package-owned immutable assets and optional domain producers. Kasane sees
 * only typed values and does not recognize application identities. */
const pocket_app_view_asset *pocket_app_view_lookup(const char *name);
typedef struct {
    const char *name;
    uint16_t width,height,variants,frames;
    ksn_result (*open)(ksn_image_port *out);
} pocket_app_image_asset;
const pocket_app_image_asset *pocket_app_image_lookup(const char *name);
#endif
