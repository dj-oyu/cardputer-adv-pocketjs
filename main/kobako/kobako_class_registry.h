#pragma once

#include "quickjs.h"
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

/* Internal, owner-task-only class cache. A descriptor is zero-initialized and
 * lives until it is forgotten/reset (normally a surface's static storage).
 * Do not edit its fields, move it, or attach it to two registries. */
typedef struct kobako_class_owner {
    JSRuntime *runtime;
    struct kobako_class_owner *next;
} kobako_class_owner_t;

typedef struct {
    kobako_class_owner_t *head;
} kobako_class_registry_t;

static inline bool kobako_class_registry_ready(kobako_class_registry_t *registry,
        JSRuntime *runtime,kobako_class_owner_t *owner,JSClassID *id) {
    assert(registry&&runtime&&owner&&id);
    if(owner->runtime==runtime)return true;
    if(!owner->runtime){owner->next=registry->head;registry->head=owner;}
    *id=0;
    owner->runtime=runtime;
    return false;
}

static inline void kobako_class_registry_forget(kobako_class_registry_t *registry,
        kobako_class_owner_t *owner) {
    assert(registry&&owner);
    kobako_class_owner_t **link=&registry->head;
    while(*link&&*link!=owner)link=&(*link)->next;
    if(*link)*link=owner->next;
    owner->runtime=NULL;owner->next=NULL;
}

static inline void kobako_class_registry_reset(kobako_class_registry_t *registry) {
    assert(registry);
    while(registry->head){
        kobako_class_owner_t *owner=registry->head;registry->head=owner->next;
        owner->runtime=NULL;owner->next=NULL;
    }
    /* Class IDs deliberately survive: old-runtime finalizers still need them
     * until JS_FreeRuntime. The next registration obtains its own runtime ID. */
}
