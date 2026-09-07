#include "scene_mem.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Plain malloc rather than an IDF heap call: the scenes have host tests and
// previews (tools/test_flower.c, tools/test_solar_sail.c, tools/preview_glass_rain.c)
// that compile them on a PC, and this file has to come with them. That is also
// why the alignment below is done by hand rather than with heap_caps_aligned_alloc.

// SCENE_ALIGN is the header's promise. malloc gives 8 at best on this target,
// so each block is over-allocated by ALIGN-1 and the raw pointer kept for
// free(): handing back an aligned view of somebody else's allocation is the
// only way to do this without an aligned_alloc the host tests would not share.
#define SCENE_ALIGN 16
static void *align_up(void *p) {
    uintptr_t v=(uintptr_t)p;
    return (void *)((v+(SCENE_ALIGN-1)) & ~(uintptr_t)(SCENE_ALIGN-1));
}

static void       *core, *core_raw;
static size_t      core_size;
static const void *core_owner;

static void       *bulk, *bulk_raw;
static size_t      bulk_size;
static const void *bulk_owner;

static void drop_bulk(void) {
    free(bulk_raw);
    bulk=NULL; bulk_raw=NULL; bulk_size=0; bulk_owner=NULL;
}

void *scene_mem(const void *owner, size_t bytes, bool *rebuild) {
    bool fresh=false;
    if(bytes>core_size) {
        // Growing throws the contents away, so this is a rebuild for whoever
        // asked -- including the current owner, which is why the size test
        // comes before the owner test rather than after it.
        //
        // realloc, not malloc: the old contents are about to be declared stale
        // anyway, but realloc can extend in place, and a scene that grows the
        // core while a 23 KB bulk sits behind it should not need room for two
        // cores at once. The aligned view is re-derived because realloc is
        // free to return a differently-misaligned address.
        void *grown=realloc(core_raw,bytes+SCENE_ALIGN-1);
        if(!grown) { if(rebuild)*rebuild=true; return NULL; }
        core_raw=grown; core=align_up(grown); core_size=bytes; fresh=true;
    }
    if(owner!=core_owner) {
        core_owner=owner;
        fresh=true;
        // A different scene is drawing now, so whatever swelled for the last
        // one is dead. Doing it here rather than in each scene is what keeps a
        // scene from having to know which other scenes exist.
        if(bulk_owner!=owner) drop_bulk();
    }
    if(fresh) memset(core,0,bytes);
    if(rebuild)*rebuild=fresh;
    return core;
}

void *scene_bulk(const void *owner, size_t bytes, bool *rebuild) {
    bool fresh=false;
    if(owner!=bulk_owner || bytes>bulk_size) {
        // Unlike the core this is not kept at the high-water mark: it is the
        // part that swells for one mode, and holding the largest mode's share
        // behind every other mode is the thing it exists to avoid.
        drop_bulk();
        bulk_raw=malloc(bytes+SCENE_ALIGN-1);
        if(!bulk_raw) { if(rebuild)*rebuild=true; return NULL; }
        bulk=align_up(bulk_raw);
        bulk_size=bytes; bulk_owner=owner; fresh=true;
    }
    if(fresh) memset(bulk,0,bytes);
    if(rebuild)*rebuild=fresh;
    return bulk;
}

void scene_mem_release(void) {
    drop_bulk();
    free(core_raw);
    core=NULL; core_raw=NULL; core_size=0; core_owner=NULL;
}
