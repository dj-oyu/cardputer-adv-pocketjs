#pragma once
#include "esp_err.h"
#include "quickjs.h"
#include <stdbool.h>
#include <stddef.h>

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

// Static import (docs/vm/eval-peak.md section 9). An app whose manifest entry
// ends in .mjs has its entry evaluated as an ES module; its `import ... from
// 'name'` reaches the app's .mjs chunks by chunk name, nothing else. The
// module loader is registered by pocket_app_load_install(), so both calls
// below are valid only after it, in the same session.
bool pocket_app_entry_is_module(void);

// Evaluates `source` as the app's module entry: compile with every static
// import resolved, then link and run the graph. Returns JS_UNDEFINED, or
// JS_EXCEPTION with the exception pending -- a module's own (SyntaxError at
// its file:line, a throw from its top level, out of memory) or a refusal (an
// unknown or script chunk named in an import, top-level await). The module's
// declarations are not global: the entry sets globalThis.frame itself.
JSValue pocket_app_eval_module(JSContext *ctx, const char *source, size_t length,
                               const char *filename);
