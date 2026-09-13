/* White-box boundary test: process-lifetime IDs have no production test hook. */
#include "../../main/ui/ds/ds_cache.c"
#include <stdio.h>

#define CHECK(x) do { \
    if (!(x)) { \
        fprintf(stderr, "cache exhaustion line %d: %s\n", __LINE__, #x); \
        return 1; \
    } \
} while (0)

int main(void) {
    ds_core core;
    ds_cache cache;
    ds_core_init(&core);
    ds_cache_init(&cache);
    ds_client app = ds_core_client(&core, DS_APP);
    ds_draw draw = {
        .kind = DS_RECT,
        .bounds = {0, 0, 8, 8},
        .clip = {0, 0, 8, 8},
        .opacity = 255,
        .data.shape = {0xffffffff, 0, 0}
    };
    ds_template component;
    CHECK(ds_cache_create(&cache, DS_APP, &draw, 1, &component) == DS_OK);
    ds_template stale_component = component;

    ds_tx tx;
    ds_placement placement = {0, 0, {0, 0, 240, 135}, 255, true};
    ds_instance instance;
    last_instance = UINT32_MAX - 1;
    CHECK(app.ops->begin(app.ctx, DS_REPLACE, &tx) == DS_OK);
    CHECK(ds_cache_instantiate(&cache, &core, tx, component, &placement,
                               &instance) == DS_OK);
    CHECK(instance.value == UINT32_MAX);
    CHECK(ds_cache_instantiate(&cache, &core, tx, component, &placement,
                               &instance) == DS_LIMIT);
    app.ops->abort(app.ctx, tx);
    CHECK(ds_cache_abort(&cache, tx) == DS_OK);
    ds_cache_init(&cache);
    CHECK(ds_cache_release(&cache, stale_component) == DS_STALE);
    CHECK(ds_cache_set_visible(&cache, &core, tx, instance, false) == DS_STALE);
    CHECK(ds_cache_create(&cache, DS_APP, &draw, 1, &component) == DS_OK);
    CHECK(app.ops->begin(app.ctx, DS_REPLACE, &tx) == DS_OK);
    CHECK(ds_cache_instantiate(&cache, &core, tx, component, &placement,
                               &instance) == DS_LIMIT);
    app.ops->abort(app.ctx, tx);

    last_template = UINT32_MAX - 1;
    CHECK(ds_cache_create(&cache, DS_APP, &draw, 1, &component) == DS_OK);
    CHECK(component.value == UINT32_MAX);
    CHECK(ds_cache_create(&cache, DS_APP, &draw, 1, &component) == DS_LIMIT);
    ds_cache_init(&cache);
    CHECK(ds_cache_create(&cache, DS_APP, &draw, 1, &component) == DS_LIMIT);

    puts("cache ID exhaustion: PASS");
    return 0;
}
