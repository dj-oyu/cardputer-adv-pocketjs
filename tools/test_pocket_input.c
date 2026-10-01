#include "pocket_input.h"
#include "pocket_api.h"
#include "pocket_kasane.h"
#include "pocket_text.h"
#include "keymap.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>
/* Generated verbatim from pocket_api.c: use production subscription semantics. */
#include "pocket_sub_impl.inc"
void host_capabilities_clear(void);
const pocket_capability_t *host_capability(const char *name);
static int64_t clock_us;
int64_t esp_timer_get_time(void){return clock_us;}
static JSContext *ctx;
static unsigned failures;
static void check(const char *code){
    JSValue value=JS_Eval(ctx,code,strlen(code),"input-test",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(value)){
        JSValue e=JS_GetException(ctx);const char *s=JS_ToCString(ctx,e);
        fprintf(stderr,"FAIL %s: %s\n",code,s?s:"?");failures++;
        if(s)JS_FreeCString(ctx,s);
        JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,value);
}
static void pump(int64_t us,uint32_t mask){clock_us=us;pocket_input_pump(mask);}

// ------------------------------------------------------------ input.keys
// The keypad FIFO, the text field and the Kasane input scope, as the test says
// they are. keymap_poll() is the real one: the edges reach keystate through the
// same call the input task makes.
static board_keyevent_t fifo[32];
static unsigned fifo_n,fifo_at;
bool board_key_event(board_keyevent_t *out){
    if(fifo_at==fifo_n){fifo_n=fifo_at=0;return false;}
    *out=fifo[fifo_at++];return true;
}
static bool text_open;
bool pocket_text_active(void){return text_open;}
static ksn_input_scope scope=KSN_INPUT_APP;
ksn_input_scope pocket_kasane_input_scope(bool host_priority){(void)host_priority;return scope;}

// Queues one edge and drains it the way input_task does. Returns what the
// shell would have been handed, so the translation can be checked unchanged.
static bool key_edge(int row,int col,bool pressed,bool overflow,keystroke_t *k){
    if(fifo_at==fifo_n) fifo_n=fifo_at=0;
    fifo[fifo_n++]=(board_keyevent_t){.row=(uint8_t)row,.col=(uint8_t)col,
                                      .pressed=pressed,.overflow=overflow};
    keystroke_t ignored;
    return keymap_poll(k?k:&ignored);
}
#define E 1,3
#define A 2,2
#define S 2,3
#define D 2,4
#define W 1,2
#define SEMI 2,11
#define SHIFT 2,1
#define ENTER 2,13
#define BACKQ 0,0

static void keys_round(void){
    // A fresh session: the pumps above have used up the install's re-base.
    pocket_input_reset();
    if(pocket_input_install(ctx,NULL)!=ESP_OK){failures++;return;}
    const pocket_capability_t *cap=host_capability("input.keys");
    if(!cap||!cap->supported||!cap->available||cap->limits){
        fprintf(stderr,"FAIL input.keys capability\n");failures++;
    }
    // Lazy: an accessor until the first read, then a plain value.
    check("var d=Object.getOwnPropertyDescriptor(input,'keys');"
          "if(typeof d.get!=='function'||'value' in d)throw Error('not lazy');"
          "var K=input.keys;d=Object.getOwnPropertyDescriptor(input,'keys');"
          "if(d.value!==K||d.get||input.keys!==K)throw Error('not replaced');"
          "if(['held','pressed','released','down'].some(n=>typeof K[n]!=='function'))"
          "throw Error('methods')");
    // The key that launched the app is down at the first turn: hidden, and its
    // release is not reported either.
    key_edge(ENTER,true,false,NULL);
    pump(2000000,0);
    check("if(K.held('enter')||K.pressed('enter')||K.down().length)throw Error('launch key seen')");
    key_edge(ENTER,false,false,NULL);
    pump(2033000,0);
    check("if(K.released('enter'))throw Error('hidden release reported')");
    // A chord; the translation the shell sees is unchanged.
    keystroke_t k;
    bool typed=key_edge(E,true,false,&k);
    if(!typed||k.len!=1||k.text[0]!='e'||k.nav!=KEY_NONE){fprintf(stderr,"FAIL translation e\n");failures++;}
    if(key_edge(SHIFT,true,false,NULL)){fprintf(stderr,"FAIL shift produced a stroke\n");failures++;}
    typed=key_edge(A,true,false,&k);
    if(!typed||k.text[0]!='A'){fprintf(stderr,"FAIL shifted translation\n");failures++;}
    key_edge(SHIFT,false,false,NULL);
    pump(2066000,0);
    check("if(!K.held('e')||!K.held('E')||!K.held('a')||K.held('shift')||!K.pressed('e')||"
          "!K.pressed('shift')||!K.released('shift')||K.down().join()!=='e,a')"
          "throw Error('chord '+K.down().join())");
    // One state per turn: an edge between pumps is not visible until the next.
    if(key_edge(E,false,false,NULL)){fprintf(stderr,"FAIL release produced a stroke\n");failures++;}
    check("if(!K.held('e')||!K.pressed('e'))throw Error('turn not stable')");
    pump(2100000,0);
    check("if(K.held('e')||!K.released('e')||K.pressed('e')||!K.held('a')||K.pressed('a'))"
          "throw Error('release edge')");
    // A tap shorter than a frame is one press and one release.
    key_edge(D,true,false,NULL);key_edge(D,false,false,NULL);
    pump(2133000,0);
    check("if(!K.pressed('d')||!K.released('d')||K.held('d'))throw Error('tap lost')");
    // Aliases name the cell; the canonical name is what down() lists.
    typed=key_edge(SEMI,true,false,&k);
    if(!typed||k.nav!=KEY_UP){fprintf(stderr,"FAIL bare ; nav\n");failures++;}
    pump(2166000,0);
    check("if(!K.held('up')||!K.held(';')||K.down().join()!=='a,;')throw Error('alias '+K.down().join())");
    key_edge(SEMI,false,false,NULL);
    // Names are checked.
    check("['nope','F1','','ee',' ','~'].forEach(n=>{var c=null;try{K.held(n)}catch(e){c=e.code}"
          "if(c!=='INVALID_ARGUMENT')throw Error('accepted '+JSON.stringify(n))});"
          "[[],[1],[null]].forEach(a=>{var c=null;try{K.pressed.apply(null,a)}catch(e){c=e.code}"
          "if(c!=='INVALID_ARGUMENT')throw Error('accepted args')});"
          "var c=null;try{K.released({toString(){return 'e'}})}catch(e){c=e.code}"
          "if(c!=='INVALID_ARGUMENT')throw Error('coerced')");
    // A FIFO overflow releases what is down, with an edge, and the late real
    // release is not a second one.
    key_edge(S,true,false,NULL);
    pump(2200000,0);
    key_edge(W,true,true,NULL);
    pump(2233000,0);
    check("if(K.held('a')||K.held('s')||!K.released('a')||!K.released('s')||!K.held('w')||"
          "!K.pressed('w'))throw Error('overflow '+K.down().join())");
    key_edge(A,false,false,NULL);key_edge(S,false,false,NULL);
    pump(2266000,0);
    check("if(K.released('a')||K.released('s')||K.down().join()!=='w')throw Error('late release')");
    // The host has the keyboard: a text field, then a blocking notice. What is
    // down stays hidden until it comes up; what is pressed meanwhile is theirs.
    text_open=true;
    key_edge(S,true,false,NULL);
    pump(2300000,0);
    check("if(K.held('w')||K.held('s')||K.pressed('s')||K.down().length)throw Error('text field')");
    text_open=false;scope=KSN_INPUT_BLOCKED;
    key_edge(D,true,false,NULL);
    pump(2333000,0);
    check("if(K.held('d')||K.pressed('d'))throw Error('blocked')");
    scope=KSN_INPUT_APP;
    pump(2366000,0);
    check("if(K.down().length||K.pressed('d'))throw Error('after host '+K.down().join())");
    key_edge(W,false,false,NULL);key_edge(S,false,false,NULL);key_edge(D,false,false,NULL);
    key_edge(E,true,false,NULL);
    pump(2400000,0);
    check("if(K.released('w')||K.released('s')||K.released('d')||!K.pressed('e')||K.down().join()!=='e')"
          "throw Error('hidden keys')");
    // A picker (main.c reports it; the guest is not pumped while it is up).
    pocket_input_keys_withhold();
    key_edge(ENTER,true,false,NULL);
    pump(2433000,0);
    check("if(K.held('enter')||K.pressed('enter')||K.held('e'))throw Error('picker')");
    key_edge(ENTER,false,false,NULL);key_edge(E,false,false,NULL);
    pump(2466000,0);
    check("if(K.released('enter')||K.released('e')||K.down().length)throw Error('picker release')");
    // Once up, a hidden key is an ordinary key again.
    key_edge(ENTER,true,false,NULL);
    pump(2480000,0);
    check("if(!K.held('enter')||!K.pressed('enter'))throw Error('unhidden')");
    key_edge(ENTER,false,false,NULL);
    pump(2490000,0);
    check("if(!K.released('enter'))throw Error('unhidden release')");
    // Suspension: the Back that put the app to sleep is not a ghost on wake,
    // and the keys the home screen got meanwhile are not edges.
    key_edge(BACKQ,true,false,NULL);
    pump(2500000,0x2000);
    check("if(!K.held('back')||!K.held('esc')||!K.held('`'))throw Error('back')");
    pocket_input_suspend();
    check("if(K.held('back'))throw Error('suspend kept back')");
    key_edge(BACKQ,false,false,NULL);key_edge(D,true,false,NULL);key_edge(D,false,false,NULL);
    key_edge(ENTER,true,false,NULL);
    pump(9000000,0);
    check("if(K.released('back')||K.pressed('d')||K.held('enter')||K.down().length)throw Error('wake')");
    key_edge(ENTER,false,false,NULL);
    pump(9033000,0);
    // The session ends with a key down; the next one does not inherit it.
    key_edge(A,true,false,NULL);
    pump(9066000,0);
    check("if(!K.held('a'))throw Error('a')");
    // (The next session's realm is the next round of main(); what carries over
    // is this file's C state, which is what is checked here.)
    pocket_input_reset();
    check("if(K.held('a')||K.down().length)throw Error('reset kept a')");
    pump(9100000,0);
    check("if(K.held('a'))throw Error('next session saw a')");
    key_edge(A,false,false,NULL);
    pump(9133000,0);
    check("if(K.released('a'))throw Error('next session saw a released')");
}

int main(void){
    for(unsigned round=0;round<2;round++){
        JSRuntime *rt=JS_NewRuntime();ctx=JS_NewContext(rt);host_capabilities_clear();
        if(pocket_input_install(ctx,NULL)!=ESP_OK)return 1;
        check("if(typeof ui!=='undefined')throw Error('node dependency');"
              "var events=[],bad=0;var s=input.onAction(e=>events.push(e));"
              "var throwing=input.onAction(()=>{bad++;throw Error('listener')});");
        pump(1000,0x4000);pump(400999,0x4000);
        check("if(events.length!==1||events[0].phase!=='press'||events[0].action!=='accept'||"
              "events[0].timeMs!==1||bad!==1||!input.held('accept'))throw Error('press/delay');");
        pump(401000,0x4000);pump(520999,0x4000);pump(521000,0x4000);pump(522000,0);
        check("if(events.map(e=>e.phase).join()!=='press,repeat,repeat,release'||bad!==1||"
              "input.held('accept'))throw Error('repeat/release/throw');s.close();s.close();");
        pump(600000,0x10);
        check("if(!input.held('up'))throw Error('held without listener');"
              "var self=0;var once=input.onAction(()=>{self++;once.close()});");
        pump(610000,0);pump(620000,0x4000);
        check("if(self!==1)throw Error('self close');"
              "var subs=[];for(var i=0;i<4;i++)subs.push(input.onAction(()=>{}));"
              "var full=false;try{input.onAction(()=>{})}catch(e){full=e.code==='LIMIT_EXCEEDED'}"
              "if(!full)throw Error('quota');subs.forEach(s=>s.close());");
        pocket_input_reset();
        check("if(input.held('accept')||input.held('up'))throw Error('reset held');");
        if(pocket_input_install(ctx,NULL)!=ESP_OK)return 1;
        check("var after=0;var current=input.onAction(()=>after++);s.close();once.close();");
        pump(1000000,0x4000);pump(1100000,0x4000);
        check("if(after!==1)throw Error('stale close or stale repeat');"
              "var unsupported=false;try{input.onKey(()=>{})}catch(e){unsupported=e.code==='UNSUPPORTED'}"
              "if(!unsupported)throw Error('onKey');");
        keys_round();
        pocket_input_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    }
    printf("input service: %s (no nodes, held, repeat boundaries, throwing/self-closing listeners, quota, reset;"
           " keys: lazy, chord, per-turn state, tap, aliases, names, overflow, host-owned turns, picker, suspend, reset)\n",
           failures?"FAIL":"PASS");
    return failures?1:0;
}
