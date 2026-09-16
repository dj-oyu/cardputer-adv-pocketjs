/* Compare native and real QuickJS calls through the production RGB565 renderer. */
#include "pocket_kasane.h"
#include "ui/kasane/ksn_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void host_capabilities_clear(void);
void *__real_calloc(size_t,size_t);
void __real_free(void *);
void *__wrap_calloc(size_t n,size_t s){return __real_calloc(n,s);}
void __wrap_free(void *p){__real_free(p);}
#define REQUIRE(c) do{if(!(c)){fprintf(stderr,"UI smoke line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static uint16_t strip[240*8],panel[240*135],reference[5][240*135];
static uint32_t transfer[5];
static ksn_ref meter;
static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *p){
    (void)ctx;memcpy(panel+y*240,p,rows*240*2);return KSN_OK;
}
static ksn_ref rect(ksn_view *v,ksn_tx tx,ksn_rect bounds,uint32_t color){
    ksn_draw d={.kind=KSN_RECT,.bounds=bounds,.clip={0,0,240,135},.opacity=255,.data.shape={color,0,0}};
    ksn_ref ref;REQUIRE(ksn_view_add(v,tx,&d,&ref)==KSN_OK);return ref;
}
static void base(ksn_view *v,ksn_tx tx,bool updated){
    REQUIRE(ksn_view_background(v,tx,0x111827ff)==KSN_OK);
    rect(v,tx,(ksn_rect){12,14,228,116},0x243247ff);
    rect(v,tx,(ksn_rect){24,26,120,32},0xe2e8f0ff);
    rect(v,tx,(ksn_rect){24,44,120,82},0x22c5b0ff);
    rect(v,tx,(ksn_rect){84,56,164,94},0xef6fbc80);
    rect(v,tx,(ksn_rect){24,102,216,108},0x111827ff);
    meter=rect(v,tx,(ksn_rect){24,102,updated?184:88,108},0xfacc15ff);
}
static void eval(JSContext *ctx,const char *code){
    JSValue result=JS_Eval(ctx,code,strlen(code),"ui-smoke",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(result)){
        JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);
        fprintf(stderr,"JS: %s\n",s?s:"exception");exit(1);
    }
    JS_FreeValue(ctx,result);pocket_kasane_end_turn();
}
static void save(const char *dir,unsigned mode,unsigned step){
    char path[1024];snprintf(path,sizeof(path),"%s/%s-%u.ppm",dir,mode?"js":"native",step);
    FILE *f=fopen(path,"wb");REQUIRE(f);fprintf(f,"P6\n240 135\n255\n");
    for(unsigned i=0;i<240*135;i++){
        unsigned p=panel[i],r=p>>11,g=(p>>5)&63,b=p&31;
        unsigned char rgb[3]={(r<<3)|(r>>2),(g<<2)|(g>>4),(b<<3)|(b>>2)};
        REQUIRE(fwrite(rgb,1,3,f)==3);
    }
    REQUIRE(fclose(f)==0);
}
int main(int argc,char **argv){
    REQUIRE(argc==2);
    for(unsigned mode=0;mode<2;mode++){
        memset(panel,0xa5,sizeof(panel));
        ksn_view *system;ksn_tx tx;ksn_render_stats stats;
        ksn_display_port port={NULL,buffer,send,240,135,8,NULL};
        REQUIRE(ksn_runtime_system_acquire(&system)==KSN_OK);
        REQUIRE(ksn_view_begin(system,KSN_REPLACE,&tx)==KSN_OK);
        rect(system,tx,(ksn_rect){212,4,228,8},0x4ade80ff);
        REQUIRE(ksn_view_submit(system,tx)==KSN_OK);
        REQUIRE(ksn_runtime_present(&port,&stats)==KSN_OK);
        ksn_app_lease lease={0};ksn_view *app=NULL;
        JSRuntime *rt=NULL;JSContext *ctx=NULL;
        if(mode){
            rt=JS_NewRuntime();REQUIRE(rt);ctx=JS_NewContext(rt);REQUIRE(ctx);
            host_capabilities_clear();REQUIRE(pocket_kasane_install(ctx,NULL)==ESP_OK);
            FILE *f=fopen("tools/kasane_ui_smoke.js","rb");REQUIRE(f);
            char source[4096];size_t n=fread(source,1,sizeof(source)-1,f);REQUIRE(!ferror(f)&&feof(f));
            source[n]=0;fclose(f);eval(ctx,source);
        }else{REQUIRE(ksn_runtime_app_attach(&lease)==KSN_OK);app=ksn_runtime_app_view(lease);}
        for(unsigned step=0;step<5;step++){
            if(mode){
                const char *calls[]={"initial()","update()","modal()","closeModal()"};
                if(step<4)eval(ctx,calls[step]);else pocket_kasane_reset();
            }else if(step==4){REQUIRE(ksn_runtime_app_detach(lease)==KSN_OK);}
            else{
                REQUIRE(ksn_view_begin(app,step==1?KSN_PATCH:KSN_REPLACE,&tx)==KSN_OK);
                if(step==1){
                    ksn_change c={.property=KSN_SET_RECT,.value.rect={24,102,184,108}};
                    REQUIRE(ksn_view_change(app,tx,meter,&c)==KSN_OK);
                }else{
                    if(step==3)REQUIRE(ksn_view_modal_close(app,tx)==KSN_OK);
                    base(app,tx,step!=0);
                    if(step==2){
                        REQUIRE(ksn_view_modal_open(app,tx,KSN_MODAL_DIM_LIVE,0x00000099,7)==KSN_OK);
                        rect(app,tx,(ksn_rect){48,34,192,100},0xeee8ddff);
                        rect(app,tx,(ksn_rect){64,48,158,54},0x334155ff);
                        rect(app,tx,(ksn_rect){116,74,176,88},0x14b8a6ff);
                    }
                }
                REQUIRE(ksn_view_submit(app,tx)==KSN_OK);ksn_runtime_app_activate(lease);
            }
            REQUIRE((mode?pocket_kasane_present(&port,&stats):ksn_runtime_present(&port,&stats))==KSN_OK);
            REQUIRE(ksn_runtime_input_scope(false)==(step==2?KSN_INPUT_MODAL:step==4?KSN_INPUT_HOST:KSN_INPUT_APP));
            if(mode){REQUIRE(memcmp(reference[step],panel,sizeof(panel))==0);REQUIRE(transfer[step]==stats.transferred_bytes);}
            else{memcpy(reference[step],panel,sizeof(panel));transfer[step]=stats.transferred_bytes;}
            if(step==1)for(unsigned y=0;y<135;y++)for(unsigned x=0;x<240;x++)
                if(y<102||y>=108||x<88||x>=184)REQUIRE(panel[y*240+x]==reference[0][y*240+x]);
            if(step==3)REQUIRE(memcmp(reference[1],panel,sizeof(panel))==0);
            if(step==1)REQUIRE(stats.transferred_bytes>0&&stats.transferred_bytes<240*135*2);
            REQUIRE(panel[4*240+212]==0x4ef0); /* Opaque SYSTEM indicator survives every APP state. */
            printf("%s step=%u bytes=%u%s\n",mode?"JS":"native",step,stats.transferred_bytes,mode?" pixels=MATCH":"");
            save(argv[1],mode,step);
            REQUIRE(ksn_runtime_present(&port,&stats)==KSN_OK&&stats.transferred_bytes==0);
        }
        if(mode){JS_FreeContext(ctx);JS_FreeRuntime(rt);}
        REQUIRE(ksn_runtime_shutdown()==KSN_OK&&ksn_runtime_reserved_bytes()==0);
    }
    puts("UI smoke: PASS (5 frames, 162000 RGB565 pixels per path, scopes, dirty update, idle, teardown)");
    return 0;
}
