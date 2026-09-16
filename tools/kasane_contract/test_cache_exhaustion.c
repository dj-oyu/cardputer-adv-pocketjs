#include "core_fixture.h"
/* White-box boundary test: process-lifetime IDs have no production test hook. */
#include "../../main/ui/kasane/ksn_cache.c"
#include <stdio.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "cache exhaustion line %d: %s\n", __LINE__, #x); \
        return 1; \
    } \
} while (0)

int main(void) {
    KSN_TEST_CORE(core,);
    ksn_cache cache;
    ksn_cache_command_block commands;ksn_cache_text_block text;
    ksn_core_init(&core);
    CHECK(ksn_cache_bind(&cache,&commands,&text)==KSN_OK);
    ksn_client app = ksn_core_client(&core, KSN_APP);
    ksn_draw draw = {
        .kind = KSN_RECT,
        .bounds = {0, 0, 8, 8},
        .clip = {0, 0, 8, 8},
        .opacity = 255,
        .data.shape = {0xffffffff, 0, 0}
    };
    ksn_template component;
    CHECK(ksn_cache_create(&cache, KSN_APP, &draw, 1, &component) == KSN_OK);
    ksn_template stale_component = component;

    ksn_tx tx;
    ksn_placement placement = {0, 0, {0, 0, 240, 135}, 255, true};
    ksn_instance instance;
    last_instance = UINT32_MAX - 1;
    CHECK(app.ops->begin(app.ctx, KSN_REPLACE, &tx) == KSN_OK);
    CHECK(ksn_cache_instantiate(&cache, &core, tx, component, &placement,
                               &instance) == KSN_OK);
    CHECK(instance.value == UINT32_MAX);
    CHECK(ksn_cache_instantiate(&cache, &core, tx, component, &placement,
                               &instance) == KSN_LIMIT);
    app.ops->abort(app.ctx, tx);
    CHECK(ksn_cache_abort(&cache, tx) == KSN_OK);
    ksn_cache_init(&cache);
    CHECK(ksn_cache_release(&cache, stale_component) == KSN_STALE);
    CHECK(ksn_cache_set_visible(&cache, &core, tx, instance, false) == KSN_STALE);
    CHECK(ksn_cache_create(&cache, KSN_APP, &draw, 1, &component) == KSN_OK);
    CHECK(app.ops->begin(app.ctx, KSN_REPLACE, &tx) == KSN_OK);
    CHECK(ksn_cache_instantiate(&cache, &core, tx, component, &placement,
                               &instance) == KSN_LIMIT);
    app.ops->abort(app.ctx, tx);

    last_template = UINT32_MAX - 1;
    CHECK(ksn_cache_create(&cache, KSN_APP, &draw, 1, &component) == KSN_OK);
    CHECK(component.value == UINT32_MAX);
    CHECK(ksn_cache_create(&cache, KSN_APP, &draw, 1, &component) == KSN_LIMIT);
    ksn_cache_init(&cache);
    CHECK(ksn_cache_create(&cache, KSN_APP, &draw, 1, &component) == KSN_LIMIT);

    puts("cache ID exhaustion: PASS");
    return 0;
}
