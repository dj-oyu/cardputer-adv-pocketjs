#include "pocket_ayatsuri_assets.h"
#include <string.h>

/* Generated from the versioned binary, consumed directly by Model validation.
 * Only the mutable Instance is expanded into guest-accounted RAM. */
static const uint8_t cloud_asset[] = {
#include "../../apps/cloud/assets/cloud_model_bytes.inc"
};
static const uint8_t fox_face_asset[] = {
#include "../../apps/fox3d/assets/fox_face_bytes.inc"
};
static const uint8_t fox_sheet_asset[] = {
#include "../../apps/foxsheet/assets/fox_sheet_bytes.inc"
};

/* Authoring comparisons are compiled into host fixtures only. A selected
 * product asset is promoted separately after visual and memory acceptance. */
#ifdef AYATSURI_FOX_COMPILED_STUDY
#include "fox_compiled_assets.inc"
#endif

bool pocket_ayatsuri_builtin_asset(const char *name,const uint8_t **data,size_t *bytes) {
    if(!name||!data||!bytes)return false;
#ifdef AYATSURI_FOX_COMPILED_STUDY
    if(fox_compiled_asset(name,data,bytes))return true;
#endif
    if(!strcmp(name,"cloud")){*data=cloud_asset;*bytes=sizeof cloud_asset;return true;}
    if(!strcmp(name,"fox-face")){*data=fox_face_asset;*bytes=sizeof fox_face_asset;return true;}
    if(!strcmp(name,"fox-sheet")){*data=fox_sheet_asset;*bytes=sizeof fox_sheet_asset;return true;}
    return false;
}
