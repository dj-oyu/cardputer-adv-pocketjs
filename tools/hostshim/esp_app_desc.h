#pragma once
// The one field main/pocket/pocket_api.c reads out of the app description, so
// that device.info() has a firmware string on a host too.
typedef struct { const char *version; } esp_app_desc_t;
const esp_app_desc_t *esp_app_get_description(void);
