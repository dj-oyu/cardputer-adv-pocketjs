#pragma once
#include "esp_err.h"
#include "quickjs.h"

// pocket.storage — the small per-app key/value store of docs/common-api.md
// section 7, published as the storage.kv capability.
//
// The backing store is NVS. Nothing is cached: every get reads flash and every
// set writes and commits it, so the value an app reads back is the value that
// survived. The 24KiB nvs partition is shared with the home settings, SKK and
// the tutorial, so the quota this enforces is smaller than section 14's
// proposal; pocket_storage.c explains the arithmetic.
//
// Everything here runs on the JS owner task.

// Registers pocket.storage on the pocket root and publishes storage.kv. Pass to
// pocketjs_guest_quickjs_install_once() after pocket_api_install.
esp_err_t pocket_storage_install(JSContext *ctx, void *user_data);

// Chooses whose store the next session opens. Section 3 requires the host, not
// JS, to settle the app identity, so this is the host's hook for it: call it
// before app_start(). A NULL or empty id restores the default owner. Takes
// effect at the next open, which is the next session.
void pocket_storage_set_owner(const char *app_id);
