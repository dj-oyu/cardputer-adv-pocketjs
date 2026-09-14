// End-to-end host contract for pocket.kasane with the real QuickJS and the
// real fixed-storage DS core/cache/modal/renderer.
#include "pocket_kasane.h"
#include <stdio.h>
#include <string.h>

void host_capabilities_clear(void);

static JSRuntime *rt;
static JSContext *ctx;
static unsigned failures;
static uint16_t strip_pixels[240*8];
static unsigned transfers;
static bool fail_once;

static void check(bool ok,const char *what) {
    printf("%s %s\n",ok?"ok  ":"FAIL",what);
    if(!ok)failures++;
}
static bool run(const char *source) {
    JSValue value=JS_Eval(ctx,source,strlen(source),"<kasane-test>",JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(value);
    if(!ok) {
        JSValue error=JS_GetException(ctx);
        const char *text=JS_ToCString(ctx,error);
        printf("    threw: %s\n",text?text:"?");
        if(text)JS_FreeCString(ctx,text);
        JS_FreeValue(ctx,error);
    }
    JS_FreeValue(ctx,value);return ok;
}
static uint16_t *get_strip(void *opaque) {(void)opaque;return strip_pixels;}
static ksn_result send_strip(void *opaque,uint16_t y,uint16_t rows,
                            const uint16_t *pixels) {
    (void)opaque;(void)y;(void)rows;(void)pixels;
    transfers++;
    if(fail_once){fail_once=false;return KSN_IO;}
    return KSN_OK;
}
static ksn_result present(ksn_render_stats *stats) {
    ksn_display_port port={.strip=get_strip,.present=send_strip,
                          .width=240,.height=135,.strip_rows=8};
    return pocket_kasane_present(&port,stats);
}
int main(void) {
    rt=JS_NewRuntime();ctx=JS_NewContext(rt);host_capabilities_clear();
    check(pocket_kasane_install(ctx,NULL)==ESP_OK,"namespace installs");
    check(run("if(kasane.features().capacity.refs!==32)throw Error('features');"
              "if(kasane.stats().active||kasane.stats().nativeBytes!==0)throw Error('lazy')"),
          "features do not allocate the native arena");
    check(!pocket_kasane_active(),"native display remains inactive after feature test");

    check(run("globalThis.tpl=kasane.cache.create(["
              "{bounds:[0,0,10,10],color:0xff0000ff},"
              "{bounds:[2,2,8,8],color:0x00ff00aa,opacity:180}]);"
              "globalThis.oldRef=null;globalThis.inst=null;"
              "globalThis.ticket=kasane.replace(tx=>{tx.background(0x000010ff);"
              "oldRef=tx.rect({bounds:[3,3,20,20],color:0xffffffff,opacity:200});"
              "let second=tx.rect({bounds:[8,8,25,25],color:0x2080ffff});"
              "tx.group(oldRef,2,190);inst=tx.instantiate(tpl,{offset:[30,20]});});"),
          "replace builds rect, group, and cached instance");
    check(pocket_kasane_active()&&pocket_kasane_has_submission(),
          "first submit claims display ownership");
    ksn_render_stats stats;transfers=0;
    check(present(&stats)==KSN_OK&&transfers==17&&stats.transferred_bytes==240*135*2,
          "initial replace presents all display bands");
    check(run("if(kasane.poll().status!=='PRESENTED')throw Error('poll')"),
          "poll reports presented state");

    check(run("ticket=kasane.patch(tx=>{oldRef.setRect(tx,[4,12,24,28]);"
              "oldRef.setColor(tx,0xf08020ff);inst.place(tx,{offset:[38,24],opacity:170});});"),
          "patch mutates refs without rebuilding topology");
    transfers=0;
    check(present(&stats)==KSN_OK&&transfers>0&&transfers<17,
          "patch transfers only dirty bands");

    check(run("globalThis.modalRef=null;ticket=kasane.replace(tx=>{"
              "tx.background(0x001020ff);tx.rect({bounds:[0,0,240,135],color:0x103050ff});"
              "tx.modal.open({backdrop:'dim-live',color:0x00000088,focus:7});"
              "modalRef=tx.rect({bounds:[40,25,200,110],color:0x80c0eedd,opacity:230});});"),
          "dim-live modal builds in a replace");
    check(present(&stats)==KSN_OK&&pocket_kasane_input_scope(false)==KSN_INPUT_MODAL,
          "modal input scope commits with presentation");
    check(run("let stale=false;try{kasane.patch(tx=>oldRef.setColor(tx,1))}"
              "catch(e){stale=e.code==='CLOSED'}if(!stale)throw Error('old ref live')"),
          "successful replace invalidates old draw refs");

    check(run("ticket=kasane.replace(tx=>{tx.background(0x101820ff);"
              "globalThis.newRef=tx.rect({bounds:[2,2,20,20],color:0xffffffff});"
              "tx.modal.close();});"),"modal close rebuilds app state");
    check(present(&stats)==KSN_OK&&pocket_kasane_input_scope(false)==KSN_INPUT_APP,
          "modal returns input to app after presentation");

    check(run("ticket=kasane.patch(tx=>newRef.setColor(tx,0x00ffffff));kasane.cancel(ticket);"
              "if(kasane.poll().status!=='DISCARDED')throw Error('cancel')"),
          "explicit cancel preserves the displayed state");
    check(!pocket_kasane_has_submission(),"cancel leaves no pending submission");

    check(run("ticket=kasane.patch(tx=>newRef.setRect(tx,[5,5,22,22]))"),
          "retry test submits a patch");
    fail_once=true;transfers=0;
    check(present(&stats)==KSN_IO&&pocket_kasane_has_submission()&&
          pocket_kasane_input_scope(false)==KSN_INPUT_BLOCKED,
          "LCD failure retains work and blocks app input");
    check(present(&stats)==KSN_OK&&!pocket_kasane_has_submission()&&
          pocket_kasane_input_scope(false)==KSN_INPUT_APP,
          "full repair retry commits and restores input");

    check(run("let rejected=false;try{kasane.patch(tx=>Promise.resolve())}"
              "catch(e){rejected=e.code==='INVALID_ARGUMENT'}"
              "if(!rejected)throw Error('thenable accepted')"),
          "thenable builders are rejected and aborted");
    check(!pocket_kasane_has_submission(),"thenable leaves no builder or submission");

    check(run("let limited=false;try{kasane.replace(tx=>{tx.background(0x000000ff);"
              "globalThis.refs=[];for(let i=0;i<33;i++)refs.push(tx.rect({"
              "bounds:[i,0,i+1,1],color:0xffffffff}));});}"
              "catch(e){limited=e.code==='LIMIT_EXCEEDED'}"
              "if(!limited)throw Error('ref limit')"),
          "draw-reference exposure is capped at 32");
    check(!pocket_kasane_has_submission(),"limit failure aborts the whole replace");

    check(run("if(kasane.stats().nativeBytes<13000)throw Error('native accounting')"),
          "stats reports the allocated native arena");
    pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    printf("%s: %u failure(s)\n",failures?"FAIL":"PASS",failures);
    return failures?1:0;
}
