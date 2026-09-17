#include "ksn_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"runtime line %d: %s\n",__LINE__,#c);return 1;} } while(0)
static long fail_after=-1;
static unsigned blocks;
static size_t maximum;
static void *allocate(size_t n,size_t size){
    if(fail_after>=0&&fail_after--==0)return NULL;
    void *p=calloc(n,size);if(p)blocks++;
    if(n*size>maximum)maximum=n*size;
    return p;
}
static void release(void *p){if(p)blocks--;free(p);}
/* Exercise the real runtime, including the process-lifetime exhaustion edge. */
#define calloc allocate
#define free release
#include "../../main/ui/kasane/ksn_runtime.c"
#undef calloc
#undef free
static uint16_t pixels[240*135],strip[240*8];
static ksn_app_lease callback_lease;
static bool callback_ok;
static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *p){
    (void)ctx;memcpy(pixels+y*240,p,rows*240*2);
    if(callback_lease.value){
        callback_ok=ksn_runtime_app_detach(callback_lease)==KSN_BUSY;
        callback_lease=(ksn_app_lease){0};
    }
    return KSN_OK;
}
int main(void){
    ksn_view *system=NULL;
    /* Control state and both banks are one block. */
    fail_after=0;
    CHECK(ksn_runtime_system_acquire(&system)==KSN_OOM);
    CHECK(!system&&!blocks&&!ksn_runtime_reserved_bytes());
    fail_after=-1;
    CHECK(ksn_runtime_system_acquire(&system)==KSN_OK);
    CHECK(blocks==1&&maximum<=KSN_RUNTIME_BASE_BUDGET&&maximum==ksn_runtime_reserved_bytes());
    CHECK(ksn_runtime_shutdown()==KSN_OK&&!blocks);system=NULL;
    /* An APP that creates the runtime gets its tail inside the same block;
     * beside an existing SYSTEM owner the tail is its own allocation, and a
     * failure of that one leaves the SYSTEM owner untouched. */
    {
        ksn_app_lease lease;void *tail=NULL;maximum=0;
        CHECK(ksn_runtime_app_attach_tail(&lease,KSN_RUNTIME_TAIL_BUDGET+1,&tail)==KSN_INVALID&&!blocks);
        fail_after=0;
        CHECK(ksn_runtime_app_attach_tail(&lease,100,&tail)==KSN_OOM&&!tail&&!blocks);
        fail_after=-1;
        CHECK(ksn_runtime_app_attach_tail(&lease,100,&tail)==KSN_OK&&tail&&blocks==1);
        CHECK(maximum==ksn_runtime_reserved_bytes()+100&&(uintptr_t)tail%KSN_RUNTIME_TAIL_ALIGN==0);
        memset(tail,0xa5,100);
        CHECK(ksn_runtime_app_detach(lease)==KSN_OK&&!blocks);
        CHECK(ksn_runtime_system_acquire(&system)==KSN_OK&&blocks==1);
        fail_after=0;
        CHECK(ksn_runtime_app_attach_tail(&lease,100,&tail)==KSN_OOM&&blocks==1&&
              !ksn_runtime_app_view(lease));
        fail_after=-1;tail=NULL;
        CHECK(ksn_runtime_app_attach_tail(&lease,100,&tail)==KSN_OK&&blocks==2);
        const unsigned char *bytes=tail;bool zero=true;
        for(unsigned i=0;i<100;i++)if(bytes[i])zero=false;
        CHECK(zero);
        CHECK(ksn_runtime_app_detach(lease)==KSN_OK&&blocks==1);
        CHECK(ksn_runtime_shutdown()==KSN_OK&&!blocks);system=NULL;
    }
    CHECK(ksn_runtime_system_acquire(&system)==KSN_OK);
    ksn_draw d={.kind=KSN_RECT,.bounds={0,0,8,8},.clip={0,0,240,135},.opacity=255,
                .data.shape={0x00ff00ff,0,0}};
    ksn_template tpl;ksn_instance instance;
    CHECK(ksn_runtime_cache_create(system,&d,1,&tpl)==KSN_OK);
    ksn_tx tx;ksn_placement p={0,0,{0,0,240,135},255,true};
    CHECK(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_view_instantiate(system,tx,tpl,&p,&instance)==KSN_OK);
    CHECK(ksn_view_submit(system,tx)==KSN_OK);
    CHECK(ksn_runtime_shutdown()==KSN_BUSY);
    ksn_display_port port={NULL,buffer,send,240,135,8,NULL};ksn_render_stats stats;
    CHECK(ksn_runtime_present(&port,&stats)==KSN_OK&&pixels[0]==0x07e0);
    CHECK(ksn_runtime_input_scope(false)==KSN_INPUT_HOST);
    ksn_app_lease old={0},current={0};
    CHECK(ksn_runtime_app_attach(&old)==KSN_OK);
    CHECK(ksn_runtime_app_attach(&current)==KSN_BUSY&&!current.value);
    CHECK(ksn_runtime_shutdown()==KSN_BUSY);
    /* APP return must not abort the independently owned SYSTEM builder. */
    CHECK(ksn_view_begin(system,KSN_PATCH,&tx)==KSN_OK);
    p.x=16;CHECK(ksn_view_place(system,tx,instance,&p)==KSN_OK);
    ksn_runtime_app_end_turn(old);
    CHECK(ksn_view_submit(system,tx)==KSN_OK);
    callback_lease=old;callback_ok=false;
    CHECK(ksn_runtime_present(&port,&stats)==KSN_OK&&callback_ok);
    CHECK(ksn_runtime_app_detach(old)==KSN_OK);
    CHECK(!ksn_runtime_app_view(old)&&blocks==4);
    CHECK(ksn_runtime_app_attach(&current)==KSN_OK&&current.value!=old.value);
    ksn_view *app=ksn_runtime_app_view(current);
    CHECK(ksn_view_begin(app,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(ksn_view_background(app,tx,0x0000ffff)==KSN_OK);
    CHECK(ksn_runtime_app_detach(old)==KSN_STALE);
    ksn_runtime_app_end_turn(old);
    CHECK(ksn_view_submit(app,tx)==KSN_OK);
    /* Detach discards pending APP work while preserving SYSTEM on the panel. */
    CHECK(ksn_runtime_app_detach(current)==KSN_OK);
    CHECK(ksn_runtime_present(&port,&stats)==KSN_OK&&pixels[0]==0&&pixels[16]==0x07e0);
    CHECK(ksn_runtime_shutdown()==KSN_OK&&!blocks);
    CHECK(!ksn_runtime_app_view(current));
    CHECK(ksn_runtime_app_attach(&old)==KSN_OK&&old.value!=current.value);
    CHECK(ksn_runtime_app_detach(current)==KSN_STALE);
    CHECK(ksn_runtime_app_detach(old)==KSN_OK&&!blocks);
    /* Exhaustion fails before allocation; it must not wrap to a stale lease. */
    last_lease=UINT32_MAX;
    current=(ksn_app_lease){77};
    CHECK(ksn_runtime_app_attach(&current)==KSN_LIMIT&&current.value==77&&!blocks);
    CHECK(ksn_runtime_system_acquire(&system)==KSN_OK);
    CHECK(ksn_runtime_shutdown()==KSN_OK&&!blocks);
    puts("native runtime: PASS (OOM, no guest, SYSTEM lifetime, stale/exhausted leases, reentry)");
    return 0;
}
