#pragma once

#include "quickjs.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

/* Internal, owner-task-only class cache. A descriptor is zero-initialized and
 * lives until it is forgotten/reset (normally a surface's static storage).
 * Do not edit its fields, move it, or attach it to two registries. */
typedef struct pocket_class_owner {
    JSRuntime *runtime;
    struct pocket_class_owner *next;
} pocket_class_owner_t;

typedef struct {
    pocket_class_owner_t *head;
} pocket_class_registry_t;

static inline bool pocket_class_registry_ready(pocket_class_registry_t *registry,
        JSRuntime *runtime,pocket_class_owner_t *owner,JSClassID *id) {
    assert(registry&&runtime&&owner&&id);
    if(owner->runtime==runtime)return true;
    if(!owner->runtime){owner->next=registry->head;registry->head=owner;}
    *id=0;
    owner->runtime=runtime;
    return false;
}

static inline void pocket_class_registry_forget(pocket_class_registry_t *registry,
        pocket_class_owner_t *owner) {
    assert(registry&&owner);
    pocket_class_owner_t **link=&registry->head;
    while(*link&&*link!=owner)link=&(*link)->next;
    if(*link)*link=owner->next;
    owner->runtime=NULL;owner->next=NULL;
}

static inline void pocket_class_registry_reset(pocket_class_registry_t *registry) {
    assert(registry);
    while(registry->head){
        pocket_class_owner_t *owner=registry->head;registry->head=owner->next;
        owner->runtime=NULL;owner->next=NULL;
    }
    /* Class IDs deliberately survive: old-runtime finalizers still need them
     * until JS_FreeRuntime. The next registration obtains its own runtime ID. */
}
