#pragma once
#include "esp_err.h"
#include "quickjs.h"

// pocket.app.load(name) -- a chunk of the running app, evaluated as a global
// script in the app's own realm (docs/api/common-api.md section 5,
// docs/vm/eval-peak.md section 7).
//
// Installed by pocket_app_install(), not by app_session.c: it is part of
// pocket.app, and a surface that joins a namespace another file owns does so
// the way pocket_workspace.c joins it, by contributing to the same lazy name.
// The chunk set is taken from app_registry_current() at install, which is the
// identity the session was started under; an app without chunks gets
// NOT_FOUND for every name.
//
// Holds no JS value, so nothing needs resetting before the guest is freed; the
// next install starts the loaded/failed state over.
esp_err_t pocket_app_load_install(JSContext *ctx);
