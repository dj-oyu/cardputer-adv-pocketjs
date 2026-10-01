/* apps/lcdcatch/lcd_catch.js as the firmware runs it, on the host: the real
 * QuickJS, the real pocket.kasane (view, validator, renderer into a 240x135
 * panel after every frame()) and the real pocket.input.keys, with key edges
 * fed through the real keymap_poll() and keystate. pocket.audio, pocket.storage
 * and pocket.random are small JS stand-ins: tone() is counted, the store is one
 * C string that survives a session, random is xorshift32-v1 as common-api.md
 * section 8.1 fixes it, seeded with a constant so a script replays exactly.
 *
 * The script (no device, no serial port):
 *  1. launch, lamp test, title;
 *  2. GAME A with a bot that walks under the lowest star, one tap per frame,
 *     until 30 points; a pause in the middle must stop the beat; then the bot
 *     stops, three misses end the game, the high score is stored;
 *  3. GAME B from the game-over screen: its beat is shorter at 0 points; mid-game
 *     Back (frame(0x2000)) stores the new GAME B high score;
 *  4. a second session reads the stored scores back and shows them on the title;
 *  5. the whole of 1-3 again from an empty store: the same results (replay).
 * With LCD_PPM=<dir> the panel is written at named moments. Guest heap is
 * charged as the device's TLSF would (the same model as
 * tools/kasane_contract/test_megademo_app_host.c).
 *
 *   bash tools/games/run_lcd_catch.sh
 */
#include "pocket_kasane.h"
#include "pocket_input.h"
#include "pocket_api.h"
#include "pocket_av.h"
#include "keymap.h"
#include "system/sys_device.h"
#include "ui/kasane/ksn_render.h"
#include "text/ksn_font.h"
#include "sound.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Generated verbatim from pocket_api.c: production subscription semantics. */
#include "pocket_sub_impl.inc"

void host_capabilities_clear(void);
int32_t pocket_av_ui_current_player(void){return 0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){(void)id;(void)out;return false;}
bool sys_device_clock_read(sys_clock_state *out){(void)out;return false;}
void sound_stream_set_observer(sound_stream_observer_fn o){(void)o;}
void sound_stream_set_observer_interval(sound_stream_observer_fn o,uint32_t n){(void)o;(void)n;}
void *__real_calloc(size_t count,size_t size);
void __real_free(void *ptr);
void __wrap_free(void *ptr){__real_free(ptr);}
void *__wrap_calloc(size_t count,size_t size){return __real_calloc(count,size);}
bool pocket_text_active(void){return false;}
int64_t esp_timer_get_time(void){return 0;}
/* Image surfaces this app does not use; linking them pulls in the SD stream. */
esp_err_t pocket_grid_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_grid_reset(void){}
bool pocket_grid_pending(void){return false;}
void pocket_grid_present_result(ksn_result r){(void)r;}
void pocket_grid_source_invalidated(uint32_t id){(void)id;}
esp_err_t pocket_video_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_video_reset(void){}
bool pocket_video_pending(void){return false;}
void pocket_video_present_result(ksn_result r){(void)r;}
esp_err_t pocket_pixel_install(JSContext *c,JSValueConst k){(void)c;(void)k;return ESP_OK;}
void pocket_pixel_reset(void){}
bool pocket_pixel_pending(void){return false;}
void pocket_pixel_present_result(ksn_result r){(void)r;}

/* The device's TLSF charge per block: request rounded up to 4 B, 12 B minimum. */
typedef struct { size_t n; size_t pad[3]; } pk_hdr;
static size_t cur_bytes,peak_bytes;
static size_t tlsf_len(size_t n){n=(n+3)&~(size_t)3;return n<12?12:n;}
static void pk_count(size_t add,size_t sub){cur_bytes+=add;cur_bytes-=sub;if(cur_bytes>peak_bytes)peak_bytes=cur_bytes;}
static void *pk_malloc(void *o,size_t n){(void)o;if(!n)return NULL;
    pk_hdr *h=malloc(sizeof *h+tlsf_len(n));if(!h)return NULL;h->n=n;pk_count(tlsf_len(n),0);return h+1;}
static void *pk_calloc(void *o,size_t c,size_t n){void *p=pk_malloc(o,c*n);if(p)memset(p,0,c*n);return p;}
static void pk_free(void *o,void *p){(void)o;if(!p)return;pk_hdr *h=(pk_hdr *)p-1;pk_count(0,tlsf_len(h->n));free(h);}
static void *pk_realloc(void *o,void *p,size_t n){
    if(!p)return pk_malloc(o,n);
    if(!n){pk_free(o,p);return NULL;}
    pk_hdr *h=(pk_hdr *)p-1;size_t old=h->n;
    pk_hdr *q=realloc(h,sizeof *q+tlsf_len(n));if(!q)return NULL;
    q->n=n;pk_count(tlsf_len(n),tlsf_len(old));return q+1;}
static size_t pk_usable(const void *p){return p?tlsf_len(((const pk_hdr *)p-1)->n):0;}
static const JSMallocFunctions PEAK_MF={pk_calloc,pk_malloc,pk_free,pk_realloc,pk_usable};

static board_keyevent_t fifo[8];
static unsigned fifo_n,fifo_at;
bool board_key_event(board_keyevent_t *out){
    if(fifo_at==fifo_n){fifo_n=fifo_at=0;return false;}
    *out=fifo[fifo_at++];return true;
}
static void edge(const char *name,bool pressed){
    int i=keymap_key_index(name);
    if(i<0){printf("no key %s\n",name);exit(2);}
    if(fifo_at==fifo_n)fifo_n=fifo_at=0;
    fifo[fifo_n++]=(board_keyevent_t){.row=(uint8_t)(i/14),.col=(uint8_t)(i%14),.pressed=pressed};
    keystroke_t k;keymap_poll(&k);
}

static JSRuntime *rt;
static JSContext *ctx;
static uint16_t strip_pixels[240*8],panel[240*135];
static unsigned exceptions,bad_present,tones,overs;
static char kv[256];static bool kv_has;
static char last_over[64];

static JSValue js_log(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *s=argc?JS_ToCString(c,argv[0]):NULL;
    if(!s)return JS_UNDEFINED;
    printf("  %s\n",s);
    if(!strncmp(s,"LCDCATCH OVER ",14)){overs++;snprintf(last_over,sizeof last_over,"%s",s+14);}
    JS_FreeCString(c,s);return JS_UNDEFINED;
}
static JSValue js_tone(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)c;(void)self;(void)argc;(void)argv;tones++;return JS_UNDEFINED;
}
static JSValue js_kv_get(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argc;(void)argv;return kv_has?JS_NewString(c,kv):JS_NULL;
}
static JSValue js_kv_set(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;const char *k=argc>1?JS_ToCString(c,argv[0]):NULL,*v=k?JS_ToCString(c,argv[1]):NULL;
    if(k&&v&&!strcmp(k,"hi")){snprintf(kv,sizeof kv,"%s",v);kv_has=true;}
    else printf("  unexpected storage.set\n");
    if(k)JS_FreeCString(c,k);
    if(v)JS_FreeCString(c,v);
    return JS_UNDEFINED;
}
static bool eval(const char *src,size_t len,const char *name){
    JSValue v=JS_Eval(ctx,src,len,name,JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(v);
    if(!ok){
        JSValue e=JS_GetException(ctx);const char *t=JS_ToCString(ctx,e);
        printf("  %s threw: %s\n",name,t?t:"?");if(t)JS_FreeCString(ctx,t);
        JS_FreeValue(ctx,e);exceptions++;
    }
    JS_FreeValue(ctx,v);
    JSContext *c;while(JS_ExecutePendingJob(rt,&c)>0){}
    pocket_kasane_end_turn();
    return ok;
}
static double num(const char *expr){
    JSValue v=JS_Eval(ctx,expr,strlen(expr),"probe.js",JS_EVAL_TYPE_GLOBAL);
    double d=-1;if(JS_IsException(v)){JS_FreeValue(ctx,JS_GetException(ctx));exceptions++;}
    else JS_ToFloat64(ctx,&d,v);
    JS_FreeValue(ctx,v);return d;
}
static uint16_t *get_strip(void *o){(void)o;return strip_pixels;}
static ksn_result send_strip(void *o,uint16_t y,uint16_t rows,const uint16_t *p){
    (void)o;memcpy(panel+y*240,p,rows*240*sizeof *p);return KSN_OK;
}
static void present(void){
    if(!pocket_kasane_needs_present())return;
    ksn_display_port port={.strip=get_strip,.present=send_strip,.width=240,.height=135,
                           .strip_rows=8,.text=&ksn_font_port};
    ksn_render_stats stats;
    if(pocket_kasane_present(&port,&stats)!=KSN_OK)bad_present++;
}
static void ppm(const char *label){
    const char *dir=getenv("LCD_PPM");if(!dir)return;
    static unsigned n;char name[512];snprintf(name,sizeof name,"%s/%02u_%s.ppm",dir,n++,label);
    FILE *o=fopen(name,"wb");if(!o)return;
    fprintf(o,"P6\n240 135\n255\n");
    for(size_t i=0;i<240*135;i++){
        uint16_t p=panel[i];
        unsigned char rgb[3]={(unsigned char)((((p>>11)&31u)*255u+15u)/31u),
            (unsigned char)((((p>>5)&63u)*255u+31u)/63u),(unsigned char)(((p&31u)*255u+15u)/31u)};
        fwrite(rgb,1,3,o);
    }
    fclose(o);
}

/* Per-frame patch size (setColor calls), split by what the frame was. */
static unsigned ops_play_max,ops_any_max,frames_patched,frames_run;
static unsigned long ops_play_sum,play_frames;
static void frame_b(unsigned buttons){
    pocket_input_pump(0);
    char call[48];snprintf(call,sizeof call,"frame(%u)",buttons);
    eval(call,strlen(call),"frame.js");
    present();
    frames_run++;
    unsigned ops=(unsigned)num("lcdCatch.ops"),mode=(unsigned)num("lcdCatch.mode");
    num("lcdCatch.ops=0");
    if(ops){frames_patched++;if(ops>ops_any_max)ops_any_max=ops;}
    if(mode==2){play_frames++;ops_play_sum+=ops;if(ops>ops_play_max)ops_play_max=ops;}
    if(cur_bytes>peak_bytes)peak_bytes=cur_bytes;
}
static void frame(void){frame_b(0);}
static void tap(const char *k){edge(k,true);edge(k,false);frame();}
static void frames(unsigned n){while(n--)frame();}
/* The bot: one tap per frame toward the lane of the lowest star. */
static const char BOT[]="(function(){const s=lcdCatch.sky;let b=-1,y=-1;"
    "for(let i=0;i<16;i++)if(s[i]&&(i&3)>y){y=i&3;b=i>>2}return b})()";
static void bot_frame(bool play){
    int lane=(int)num(BOT),pl=(int)num("lcdCatch.pl");
    if(play&&lane>=0&&lane<pl)tap("a");
    else if(play&&lane>pl)tap("d");
    else frame();
}

static const char STUB[]=
    "globalThis.console={log:globalThis.__log};"
    "globalThis.pocket={kasane:globalThis.kasane,input:globalThis.input,"
    "capabilities:{get:n=>({name:n,supported:true,available:true})},"
    "audio:{tone(s){__tone(s.frequencyHz,s.durationMs);return Promise.resolve()}},"
    "storage:{get(k){const s=__kvGet(k);return Promise.resolve(s===null?null:{value:JSON.parse(s),revision:1})},"
    "set(k,v){__kvSet(k,JSON.stringify(v));return Promise.resolve({revision:1})}},"
    "random:{seed:()=>20260929,create(s){let x=(s>>>0)||0x6D2B79F5;"
    "const n=()=>{x^=x<<13;x>>>=0;x^=x>>>17;x^=x<<5;return x>>>=0};"
    "return{nextUint32:n,nextFloat:()=>(n()>>>8)/16777216}}}};";

static char src[1<<15];static size_t src_n;
static size_t heap_eval,heap_eval_peak,heap_run_peak,commands;
static void open_session(void){
    rt=JS_NewRuntime2(&PEAK_MF,NULL);
    JS_SetMemoryLimit(rt,163840);          /* main/app_session.c heap_limit */
    if(163840/2<JS_GetGCThreshold(rt))JS_SetGCThreshold(rt,163840/2);
    ctx=JS_NewContext(rt);host_capabilities_clear();
    pocket_kasane_install(ctx,NULL);
    pocket_input_install(ctx,NULL);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));
    JS_SetPropertyStr(ctx,g,"__tone",JS_NewCFunction(ctx,js_tone,"tone",2));
    JS_SetPropertyStr(ctx,g,"__kvGet",JS_NewCFunction(ctx,js_kv_get,"kvGet",1));
    JS_SetPropertyStr(ctx,g,"__kvSet",JS_NewCFunction(ctx,js_kv_set,"kvSet",2));
    JS_FreeValue(ctx,g);
    eval(STUB,strlen(STUB),"stub.js");
    JS_RunGC(rt);
    size_t before=cur_bytes;peak_bytes=cur_bytes;
    edge("enter",true);                    /* the key that launched it */
    eval(src,src_n,"lcd_catch.js");present();
    heap_eval_peak=peak_bytes-before;
    JS_RunGC(rt);heap_eval=cur_bytes-before;
    commands=(size_t)num("pocket.kasane.stats().displayed.commands");
    frame();edge("enter",false);frame();
    peak_bytes=cur_bytes;
}
static size_t heap_base;
static void close_session(void){
    heap_run_peak=peak_bytes;
    pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
}

typedef struct { unsigned score_a,ticks_a,score_b,tick_b0,tick_a0; char kv[256]; } result;
static unsigned fails;
#define CHECK(c,msg) do{if(!(c)){fails++;printf("  FAIL: %s\n",msg);}}while(0)

/* How many frames one beat takes right now: frames until ticks moves twice. */
static unsigned beat(void){
    unsigned t0=(unsigned)num("lcdCatch.ticks"),n=0;
    while((unsigned)num("lcdCatch.ticks")==t0&&n<100){frame();n++;}
    t0=(unsigned)num("lcdCatch.ticks");n=0;
    while((unsigned)num("lcdCatch.ticks")==t0&&n<100){bot_frame(true);n++;}
    return n;
}

static result play_script(bool shots){
    result r={0};
    open_session();
    heap_base=cur_bytes;
    if(shots)ppm("lamp");
    frames(30);
    CHECK(num("lcdCatch.mode")==1,"title after the lamp test");
    if(shots)ppm("title");
    /* GAME A */
    tap("1");
    CHECK(num("lcdCatch.mode")==2&&num("lcdCatch.game")==0,"1 starts GAME A");
    r.tick_a0=beat();
    unsigned guard=0;bool paused_once=false,shot_play=false,shot_play2=false;
    while(num("lcdCatch.score")<30&&num("lcdCatch.mode")!=4&&guard++<6000){
        bot_frame(true);
        double s=num("lcdCatch.score");
        if(shots&&!shot_play&&s>=6&&num("lcdCatch.sky.reduce((a,b)=>a+b,0)")>=3){ppm("play");shot_play=true;}
        if(shots&&!shot_play2&&s>=22&&num("lcdCatch.sky.reduce((a,b)=>a+b,0)")>=3){ppm("play_fast");shot_play2=true;}
        if(!paused_once&&s>=15){
            tap("tab");
            unsigned t0=(unsigned)num("lcdCatch.ticks");
            frames(90);
            CHECK(num("lcdCatch.paused")==1&&(unsigned)num("lcdCatch.ticks")==t0,"pause stops the beat");
            if(shots)ppm("pause");
            tap("tab");paused_once=true;
            CHECK(num("lcdCatch.paused")==0,"tab resumes");
        }
    }
    CHECK(num("lcdCatch.misses")==0,"the bot catches every star up to 30");
    CHECK(num("lcdCatch.score")>=30,"GAME A reaches 30");
    /* The bot stops: three misses and game over. */
    guard=0;bool shot_miss=false;
    while(num("lcdCatch.mode")!=4&&guard++<3000){
        frame();
        if(shots&&!shot_miss&&num("lcdCatch.mode")==3&&num("lcdCatch.misses")==2&&((int)num("lcdCatch.fc")>>2&1)){ppm("miss");shot_miss=true;}
    }
    frames(5);
    r.score_a=(unsigned)num("lcdCatch.score");r.ticks_a=(unsigned)num("lcdCatch.ticks");
    CHECK(num("lcdCatch.mode")==4&&num("lcdCatch.misses")==3,"three misses end GAME A");
    CHECK(num("lcdCatch.hi[0]")==r.score_a,"GAME A high score is the final score");
    if(shots)ppm("game_over");
    /* GAME B from the game-over screen */
    tap("2");
    CHECK(num("lcdCatch.mode")==2&&num("lcdCatch.game")==1,"2 starts GAME B");
    r.tick_b0=beat();
    guard=0;
    while(num("lcdCatch.score")<20&&guard++<6000)bot_frame(true);
    r.score_b=(unsigned)num("lcdCatch.score");
    if(shots)ppm("game_b");
    frame_b(0x2000);                       /* Back: the save turn */
    CHECK(kv_has,"storage written");
    snprintf(r.kv,sizeof r.kv,"%s",kv);
    close_session();
    return r;
}

int main(void){
    FILE *f=fopen("apps/lcdcatch/lcd_catch.js","rb");
    if(!f){printf("cannot open apps/lcdcatch/lcd_catch.js\n");return 2;}
    src_n=fread(src,1,sizeof src-1,f);fclose(f);src[src_n]=0;

    printf("session 1: GAME A, pause, game over, GAME B, Back\n");
    result a=play_script(true);
    size_t eval_heap=heap_eval,eval_peak=heap_eval_peak,run_peak=heap_run_peak,cmds=commands;
    unsigned play_max=ops_play_max,any_max=ops_any_max;
    printf("  GAME A: score %u in %u beats, first beat %u frames; GAME B: first beat %u frames, score %u at Back\n",
           a.score_a,a.ticks_a,a.tick_a0,a.tick_b0,a.score_b);
    printf("  stored: %s\n",a.kv);
    char want[64];snprintf(want,sizeof want,"[%u,%u]",a.score_a,a.score_b);
    CHECK(!strcmp(a.kv,want),"Back stores both high scores");
    CHECK(a.tick_a0==13&&a.tick_b0==10,"GAME B starts on a shorter beat");

    printf("session 2: the stored scores come back\n");
    open_session();frames(30);
    CHECK(num("lcdCatch.hi[0]")==a.score_a&&num("lcdCatch.hi[1]")==a.score_b,"high scores read back");
    CHECK(num("lcdCatch.mode")==1,"title");
    tap("s");frames(2);
    CHECK(num("lcdCatch.game")==1,"s selects GAME B on the title");
    tap("s");
    ppm("title_hi");
    close_session();

    printf("session 3: the same script from an empty store\n");
    kv_has=false;kv[0]=0;
    result b=play_script(false);
    CHECK(a.score_a==b.score_a&&a.ticks_a==b.ticks_a&&a.score_b==b.score_b&&!strcmp(a.kv,b.kv),
          "same seed, same script, same game");

    printf("source %zu B; kasane commands %zu (limit 80)\n",src_n,cmds);
    printf("guest heap (TLSF model): after eval %zu B, peak during eval %zu B, whole-session peak %zu B "
           "(limit 163840)\n",eval_heap,eval_peak,run_peak);
    printf("patch: most operations (setColor, setRect, place) in one frame %u (any), %u (during play); "
           "mean during play %.2f per frame; frames patched %u of %u\n",any_max,play_max,
           play_frames?(double)ops_play_sum/play_frames:0.0,frames_patched,frames_run);
    printf("tones %u, game-overs %u, last: %s; exceptions %u, bad presents %u\n",
           tones,overs,last_over,exceptions,bad_present);
    bool pass=!fails&&!exceptions&&!bad_present;
    printf("%s\n",pass?"LCDCATCH_HOST PASS":"LCDCATCH_HOST FAIL");
    return pass?0:1;
}
