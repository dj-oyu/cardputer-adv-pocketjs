#include "pocket_ayatsuri_assets.h"
#include <string.h>

/* Generated from the versioned binary, consumed directly by Model validation.
 * Only the mutable Instance is expanded into guest-accounted RAM. */
static const uint8_t cloud_asset[] = {
#include "../../apps/cloud/assets/cloud_model_bytes.inc"
};

bool pocket_ayatsuri_builtin_asset(const char *name,const uint8_t **data,size_t *bytes) {
    if(!name||!data||!bytes||strcmp(name,"cloud"))return false;
    *data=cloud_asset;*bytes=sizeof cloud_asset;return true;
}
