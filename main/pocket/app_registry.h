#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The app registration record of docs/common-api.md section 3, as C data.
//
// Section 3 says the host settles the app identity and that JS never claims
// another app's storage. Until now that was one ternary in main.c —
// `source==pet_start ? "local.pet" : "local.default"` — which is an identity
// derived from a pointer, and there was nowhere to put the rest of the record:
// the API range an app is written against, the capabilities it needs before it
// is worth starting, and what the host lets it reach.
//
// This file is that record. It is deliberately free of ESP-IDF and of QuickJS:
// every decision below is a pure function of the manifest and its arguments, so
// tools/test_app_registry.c settles the API range grammar, the admission rule
// and the permission matrix on the host rather than by flashing a board.
//
// Registration is still build-time. Section 3 asks only that an installed app
// use the same information model, and nothing here assumes the table is static
// except the table itself.

// Section 2 keeps the existing apps on the old execution mode and requires an
// explicit move to the common API. The runtime says which one an app is, and
// the API range is checked only for the new one: a legacy program never reads
// pocket.apiVersion, so refusing it for a version range it does not use would
// be a rule with no subject.
typedef enum {
    APP_RUNTIME_LEGACY = 0,   // ui.createNode and globalThis.frame
    APP_RUNTIME_POCKET,       // pocket.* — the surface of docs/common-api.md
} app_runtime_t;

// What the host lets an app do with the works library (section 3's `access`,
// section 7's workspace). The levels are ordered, and the order is the whole
// rule: a level may do everything the level below it may.
typedef enum {
    APP_WORKS_NONE = 0,   // pocket.workspace exists and refuses every call
    APP_WORKS_SELF,       // create, and read/save/run what this app created
    APP_WORKS_PICK,       // plus the picker, and whatever the person chose in it
} app_works_t;

typedef enum {
    APP_WORK_PICK = 0,
    APP_WORK_CREATE,
    APP_WORK_READ,
    APP_WORK_SAVE,
    APP_WORK_RUN,
} app_work_op_t;

typedef struct {
    const char        *id;         // "local.pet" — the host's name, never JS's
    const char        *title;
    const char        *entry;      // for the record; this build embeds sources
    app_runtime_t      runtime;
    const char        *api;        // ">=0.1.0 <0.2.0", or NULL for "any"
    const char *const *required;   // NULL-terminated capability names
    const char *const *optional;   // NULL-terminated; absence is not a refusal
    app_works_t        works;
} app_manifest_t;

// The identity a work launched from the library runs under. It is its own app
// rather than the Playground's: section 3 wants the Playground's runs to have
// their own execution owner, and a work that could write the Playground's
// storage would be exactly what that sentence forbids.
#define APP_ID_WORK      "local.work"
#define APP_ID_DEFAULT   "local.hello"

// A stable 32-bit name for an app id, for the stores that have to write down
// whose something is and have no room for the id itself. FNV-1a, chosen for its
// size: the works index is not adversarial, and an app that could name another
// app's id already has that app's identity.
uint32_t app_registry_owner_hash(const char *id);

// NULL when no app carries that id.
const app_manifest_t *app_registry_find(const char *id);

// Which manifest the next session runs under. Section 3 puts this with the
// host, so it is the host's hook: call it before app_start(), the way
// pocket_storage_set_owner() was called before it. A NULL or unknown id
// selects APP_ID_DEFAULT, because a session with no identity would otherwise
// inherit the last one's.
void                  app_registry_select(const char *id);
const app_manifest_t *app_registry_current(void);

// Whether `version` satisfies `range`. The grammar is the one section 3 writes:
// space-separated clauses of an operator and a dotted triple, e.g.
// ">=0.1.0 <0.2.0". Operators are >=, <=, >, <, = and ==. A NULL or empty range
// admits everything; a clause this cannot parse admits nothing, because a range
// nobody can read is not a range an app should be started on.
bool app_registry_api_ok(const char *range, const char *version);

// Whether the manifest names `capability` in either list — required or
// optional. Both count, because the question this answers is not "may it" but
// "will it want to", and an optional capability an app never gets is an app
// running with less than it asked for.
//
// This is deliberately readable at the very top of a session, BEFORE the guest
// heap is created: the declaration is host data settled by app_registry_select()
// at launch, and nothing about it needs a realm, a capability registration or an
// install pass. That ordering is the point. A capability whose acquisition costs
// tens of KB of internal DRAM — the radio is the measured case, ~27 KB for
// esp_wifi_init and ~21 KB more for the netif and event loop — cannot be taken
// once a guest holds 100 KB+; it does not degrade, it refuses with
// ESP_ERR_NO_MEM. Reserving by ordering (take it first, then size the guest into
// what is left) costs nothing to an app that never asks, which a dedicated heap
// region would not. Whoever implements that ordering asks this function.
bool app_registry_wants(const app_manifest_t *manifest, const char *capability);

// Section 3: an app whose API range or required capabilities do not fit is
// shown before it starts rather than failing somewhere inside its own code.
// Returns true to admit. On a refusal `reason` receives a short upper-case line
// for the screen ("NEEDS sensors.imu"), which is the form main.c's error line
// and the USB scripts already read.
//
// `supported` answers section 2's supported flag for one name. It is a callback
// so that this file need not know pocket_api.c exists — and so the host test
// can answer for capabilities no host has.
bool app_registry_admit(const app_manifest_t *manifest, const char *api_version,
                        bool (*supported)(const char *name, void *user), void *user,
                        char *reason, size_t reason_size);

// The permission matrix of section 3, as one function. `owned` is "this app
// created this work", `picked` is "the person chose it in the host's picker
// during this session". Both are facts the host holds; neither is anything JS
// can assert.
bool app_registry_may_work(app_works_t level, app_work_op_t op,
                           bool owned, bool picked);

// Section 7: a save onto an existing work needs an explicit choice and a
// revision check. The choice is the picker; this is the other half, and it is
// here rather than inside the workspace so the rule is stated once and tested.
static inline bool app_registry_save_needs_revision(bool owned) { return !owned; }

// Copies a work title into `out`, refusing what a title may not be: empty,
// longer than the buffer, malformed UTF-8, or carrying a control character
// (a newline in a title would draw over the row below it in the picker).
// Returns the byte length written, or 0 on a refusal. Never truncates: section
// 4 refuses to round a request quietly, and a title silently cut in half is the
// same fault wearing different clothes.
size_t app_registry_clean_title(const char *in, size_t length,
                                char *out, size_t out_size);
