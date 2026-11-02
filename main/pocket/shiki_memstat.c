// The allocation counters of shiki_memstat.h. One task (the UI task) does all
// of this; no lock.
#include "shiki_memstat.h"
#ifdef CONFIG_POCKET_SHIKI_MEMSTAT
#include "esp_heap_caps.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LIVE 128   // allocations alive at once that free() can attribute
#define SITES 48

typedef struct { uint16_t file,line; uint32_t n,cur,peak,max; } site_t;
typedef struct { void *p; uint32_t bytes; uint8_t site; } live_t;
static site_t sites[SITES];
static live_t live[LIVE];
static unsigned nsites;
static uint32_t cur,peak,count,overflow,lg_min=UINT32_MAX,free_min=UINT32_MAX,lg_at_peak,free_at_peak;

static int site_of(int file,int line){
    for(unsigned k=0;k<nsites;k++)if(sites[k].file==file&&sites[k].line==line)return (int)k;
    if(nsites==SITES)return -1;
    sites[nsites]=(site_t){(uint16_t)file,(uint16_t)line,0,0,0,0};
    return (int)nsites++;
}
static void *note(int file,int line,void *p){
    if(!p)return p;
    const uint32_t b=(uint32_t)heap_caps_get_allocated_size(p);
    const int s=site_of(file,line);
    unsigned k=0;
    while(k<LIVE&&live[k].p)k++;
    if(k==LIVE||s<0){overflow++;return p;}
    live[k]=(live_t){p,b,(uint8_t)s};
    site_t *t=&sites[s];
    t->n++; t->cur+=b; if(t->cur>t->peak)t->peak=t->cur; if(b>t->max)t->max=b;
    count++; cur+=b;
    // The internal heap right after this allocation: what is left beside it.
    const uint32_t fr=(uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    const uint32_t lg=(uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    if(fr<free_min)free_min=fr;
    if(lg<lg_min)lg_min=lg;
    if(cur>peak){peak=cur;free_at_peak=fr;lg_at_peak=lg;}
    return p;
}
void *shiki_ms_malloc(int file,int line,size_t n){return note(file,line,malloc(n));}
void *shiki_ms_calloc(int file,int line,size_t c,size_t n){return note(file,line,calloc(c,n));}
void shiki_ms_free(void *p){
    if(!p)return;
    for(unsigned k=0;k<LIVE;k++)if(live[k].p==p){
        sites[live[k].site].cur-=live[k].bytes;
        cur-=live[k].bytes;
        live[k]=(live_t){0};
        break;
    }
    free(p);
}
void shiki_memstat_report(void){
    static const char *const name[]={"proc","grid","derby"};
    ESP_LOGI("shiki","SHIKI_MEM total peak=%lu cur=%lu n=%lu overflow=%lu free_at_peak=%lu lg_at_peak=%lu free_min=%lu lg_min=%lu",
             (unsigned long)peak,(unsigned long)cur,(unsigned long)count,(unsigned long)overflow,
             (unsigned long)free_at_peak,(unsigned long)lg_at_peak,
             (unsigned long)(free_min==UINT32_MAX?0:free_min),(unsigned long)(lg_min==UINT32_MAX?0:lg_min));
    for(unsigned k=0;k<nsites;k++)
        ESP_LOGI("shiki","SHIKI_MEM site %s:%u n=%lu peak=%lu max=%lu cur=%lu",name[sites[k].file],sites[k].line,
                 (unsigned long)sites[k].n,(unsigned long)sites[k].peak,(unsigned long)sites[k].max,(unsigned long)sites[k].cur);
}
void shiki_memstat_reset(void){
    memset(sites,0,sizeof sites);memset(live,0,sizeof live);
    nsites=0;cur=peak=count=overflow=lg_at_peak=free_at_peak=0;lg_min=free_min=UINT32_MAX;
}
#endif
