#include "app_view_provider.h"
#include "apps/apps.h"
#include <string.h>

/* Composition root: Kasane consumes only the type-erased provider contract. */
static const kobako_app_view_provider *const providers[]={&kobako_app_music_view};

const kobako_app_view_provider *kobako_app_view_provider_lookup(const char *name){
    if(!name)return NULL;
    for(size_t i=0;i<sizeof(providers)/sizeof(providers[0]);i++)
        if(strcmp(providers[i]->name,name)==0)return providers[i];
    return NULL;
}
