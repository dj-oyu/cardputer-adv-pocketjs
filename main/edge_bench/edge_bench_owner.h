#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
typedef struct {uint32_t generation,run;bool owned,bound,quarantine,logged;} edge_owner;
static inline bool edge_owner_begin(edge_owner *o){
 if(o->owned||o->generation==UINT32_MAX)return false;
 o->generation++;o->run=0;o->owned=true;o->bound=false;o->quarantine=false;o->logged=false;return true;
}
static inline bool edge_owner_bind(edge_owner *o,uint32_t run,uint32_t instance){
 if(!o->owned||o->bound||o->quarantine||!run||instance!=o->generation)return false;
 o->run=run;o->bound=true;return true;
}
static inline bool edge_owner_release(edge_owner *o,uint32_t instance){
 if(!o->owned||o->quarantine||instance!=o->generation)return false;
 o->owned=false;return true;
}
