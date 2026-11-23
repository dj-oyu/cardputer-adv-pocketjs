#pragma once
#include "esp_err.h"
#include "quickjs.h"

// kobako.storage — the small per-app key/value store of docs/api/common-api.md
// section 7, published as the storage.kv capability.
//
// The backing store is NVS. Nothing is cached: every get reads flash and every
// set writes and commits it, so the value an app reads back is the value that
// survived. The 24KiB nvs partition is shared with the home settings, SKK and
// the tutorial, so the quota this enforces is smaller than section 14's
// proposal; kobako_storage.c explains the arithmetic.
//
// Everything here runs on the JS owner task.

// Registers kobako.storage on the kobako root and publishes storage.kv. Pass to
// kobako_guest_quickjs_install_once() after kobako_api_install.
esp_err_t kobako_storage_install(JSContext *ctx, void *user_data);

// Chooses whose store the next session opens. Section 3 requires the host, not
// JS, to settle the app identity, so this is the host's hook for it: call it
// before app_start(). A NULL or empty id restores the default owner. Takes
// effect at the next open, which is the next session.
void kobako_storage_set_owner(const char *app_id);
