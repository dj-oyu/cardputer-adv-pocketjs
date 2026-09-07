// The SESSION lifetime of pocket.input.text, on a host, with the real QuickJS.
// Host only (WSL: gcc is not on the Windows side):
//
//   wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs &&
//                    bash tools/build_pocket_text_test.sh && /tmp/test-pocket-text"
//
// tools/test_textfield.c settles what one keystroke MEANS, and it does that
// without a session existing at all -- which is exactly why 44 assertions there
// could not see this bug. Everything dangerous about pocket_text.c is in the
// other half: the session is a single calloc of struct+buffer, three guest
// callbacks are fired from inside a keystroke, and a listener may free that
// allocation by calling close() while the host is still standing in the middle
// of it. That is not a rule about text; it is a rule about lifetime, and it
// needs a JS engine to provoke.
//
// So this links the REAL main/pocket/pocket_text.c against the REAL quickjs-ng
// and stubs only the board (tools/hostshim). Under -fsanitize=address the
// use-after-free is a report with a stack, instead of a LoadProhibited with no
// symbols four seconds after the keystroke that caused it.
#include "pocket_text.h"
#include "keymap.h"
#include "board.h"
#include "skk_session.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

void host_ime_present(bool present);
void host_capabilities_clear(void);

static unsigned failures;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if(!ok) failures++;
}

// ---- the engine ------------------------------------------------------------

static JSRuntime *rt;
static JSContext *ctx;

// What the JS side reports, so a callback that ran can be proved to have run.
static char   log_buf[4096];
static size_t log_len;

static JSValue js_note(JSContext *c, JSValueConst self, int argc,
                       JSValueConst *argv) {
    (void)self;
    if(argc>0) {
        const char *s=JS_ToCString(c,argv[0]);
        if(s) {
            int n=snprintf(log_buf+log_len,sizeof log_buf-log_len,"%s;",s);
            if(n>0) log_len+=(size_t)n;
            printf("    js: %s\n",s);
            JS_FreeCString(c,s);
        }
    }
    return JS_UNDEFINED;
}
static bool logged(const char *needle) { return strstr(log_buf,needle)!=NULL; }

static void engine_up(void) {
    rt=JS_NewRuntime();
    ctx=JS_NewContext(rt);
    JSValue g=JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx,g,"note",JS_NewCFunction(ctx,js_note,"note",1));
    JS_FreeValue(ctx,g);
    log_len=0; log_buf[0]='\0';
    host_capabilities_clear();
    pocket_text_install(ctx,NULL);
}
static void engine_down(void) {
    pocket_text_reset();
    JS_FreeContext(ctx); JS_FreeRuntime(rt);
    ctx=NULL; rt=NULL;
}

static bool run(const char *src) {
    JSValue v=JS_Eval(ctx,src,strlen(src),"<test>",JS_EVAL_TYPE_GLOBAL);
    bool ok=!JS_IsException(v);
    if(!ok) {
        JSValue e=JS_GetException(ctx);
        const char *t=JS_ToCString(ctx,e);
        printf("    threw: %s\n",t?t:"?");
        if(t) JS_FreeCString(ctx,t);
        JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,v);
    return ok;
}

// ---- keystrokes, in the shape main/hal/keymap.c produces -------------------

static void key(const char *utf8) {
    keystroke_t k;
    memset(&k,0,sizeof k);
    k.len=(uint8_t)strlen(utf8);
    memcpy(k.text,utf8,k.len);
    pocket_text_key(&k);
}
static void token(const char *name) {          // a "\0name" key
    keystroke_t k;
    memset(&k,0,sizeof k);
    size_t n=strlen(name);
    memcpy(k.text+1,name,n);
    k.len=(uint8_t)(n+1);
    pocket_text_key(&k);
}
// One keystroke the IME answers with a commit -- the path phase C of
// apps/textcheck drives, where the commit is computed before the callback and
// the callback then destroys the field it was computed for.
static void key_committing(const char *commit) {
    ime_t *im=skk_session();
    im->on=true;
    im->next=IME_TEXT;
    im->commit_len=strlen(commit);
    memcpy(im->commit,commit,im->commit_len);
    key("a");
    im->next=IME_NONE;
}

// The overlay runs over every strip, which is also a read of the live session:
// a session freed underneath the module would be found here too.
static void paint_frame(void) {
    static uint16_t strip[LCD_W*STRIP_H];
    for(int y=0;y<LCD_H;y+=STRIP_H) {
        int rows=(y+STRIP_H<=LCD_H)?STRIP_H:(LCD_H-y);
        memset(strip,0,sizeof strip);
        pocket_text_overlay(strip,y,rows);
    }
}

#define RECT "rect:{x:8,y:56,width:224,height:20}"

// Each case is its own function so one can be run alone -- pass its number on
// the command line -- which is how a report is narrowed to the keystroke that
// produced it.
static void case1(void) {
    // ---- 1. the crash: close() and reopen() from inside onEdit -------------
    //
    // apps/textcheck phase C, exactly. The listener frees the session the host
    // is in the middle of firing, and then makes the allocator hand the same
    // size class straight back -- so a stale `s->ctx` read after the call is
    // not poison but a live-looking pointer built out of the new session's
    // field. On the board that is a LoadProhibited; here it is a report.
    engine_up();
    check(run(
        "var swapped=false;"
        "function reopen(){ input.text.open({" RECT ",maxBytes:24,ime:'on',"
        "  onEdit:function(e){ note('EDIT-D ['+e.text+']'); }});"
        "  note('OPEN-D'); }"
        "var c=input.text.open({" RECT ",maxBytes:24,ime:'on',"
        "  onEdit:function(e){ note('EDIT-C ['+e.text+']');"
        "    if(swapped) return; swapped=true; c.close(); reopen(); },"
        "  onCancel:function(){ note('CANCEL-C'); }});"
        "note('OPEN-C');"),"phase C opens");
    key_committing("\xe3\x81\x82");                 // an IME commit of あ
    check(logged("EDIT-C"),"onEdit ran for the first session");
    check(logged("OPEN-D"),"the listener's replacement session opened");
    check(pocket_text_active(),"a session is live after the swap");
    paint_frame();
    // The replacement is a DIFFERENT field: the commit computed for the first
    // one must not have landed in it.
    key("x");
    check(logged("EDIT-D [x]"),
          "the replacement holds only what was typed into it");
    engine_down();

}

static void case2(void) {
    // ---- 2. the same swap without the IME ---------------------------------
    // The plain tf_key() path reaches fire_edit() from the other side of
    // pocket_text_key(). Same lifetime, different caller.
    engine_up();
    check(run(
        "var n=0;"
        "var c=input.text.open({" RECT ",maxBytes:24,ime:'off',"
        "  onEdit:function(e){ note('EDIT '+(++n)+' ['+e.text+']');"
        "    if(n===1){ c.close(); input.text.open({" RECT ",maxBytes:24,"
        "      ime:'off', onEdit:function(e2){ note('EDIT2 ['+e2.text+']'); }});"
        "    } }});"),"a latin session opens");
    key("k");
    check(logged("EDIT 1 [k]"),"the first edit fired");
    check(pocket_text_active(),"the replacement is live");
    key("z");
    check(logged("EDIT2 [z]"),"the replacement takes the next key");
    paint_frame();
    engine_down();

}

static void case3(void) {
    // ---- 3. close() with no reopen, from inside onEdit --------------------
    // Nothing takes the freed block back, so this is the case a poisoned read
    // would catch even without a reuse -- and the one that must leave the
    // module with no session rather than a dangling one.
    engine_up();
    check(run(
        "var c=input.text.open({" RECT ",ime:'off',"
        "  onEdit:function(){ note('EDIT'); c.close(); }});"),"opens");
    key("q");
    check(logged("EDIT"),"the edit fired");
    check(!pocket_text_active(),"the session is gone");
    paint_frame();
    check(run("var d=input.text.open({" RECT "}); note('REOPEN-OK');"),
          "open() works again after a listener closed the last one");
    check(logged("REOPEN-OK"),"and it really ran");
    engine_down();

}

static void case4(void) {
    // ---- 4. the ending paths, with a listener that reopens ----------------
    // Submit and cancel detach BEFORE the callback, so a reopen from inside is
    // legal and must not be answered BUSY -- and the old session is destroyed
    // after the callback, which is only safe because close() inside it finds
    // `live` already NULL and does nothing.
    engine_up();
    check(run(
        "input.text.open({" RECT ",ime:'off',initial:'hi',"
        "  onSubmit:function(e){ note('SUBMIT ['+e.text+']');"
        "    input.text.open({" RECT ",ime:'off',"
        "      onCancel:function(){ note('CANCEL2'); }});"
        "    note('REOPENED'); }});"),"opens with an onSubmit");
    key("\n");
    check(logged("SUBMIT [hi]"),"Enter submitted the text");
    check(logged("REOPENED"),"a session opened from inside onSubmit");
    check(pocket_text_active(),"and it is the live one");
    token("esc");
    check(logged("CANCEL2"),"Escape cancelled the replacement");
    check(!pocket_text_active(),"nothing is live");
    engine_down();

}

static void case5(void) {
    // ---- 5. a listener that closes AND throws -----------------------------
    // The throw is reported through the context the host cached; a reload of
    // s->ctx to report it would be a read of the block the close() freed.
    engine_up();
    check(run(
        "var c=input.text.open({" RECT ",ime:'off',"
        "  onEdit:function(){ note('EDIT'); c.close();"
        "                     throw new Error('boom'); }});"),"opens");
    key("w");
    check(logged("EDIT"),"the listener ran before it threw");
    check(!pocket_text_active(),"the throw did not resurrect the session");
    engine_down();

}

static void case6(void) {
    // ---- 6. a TextSession object outliving its session --------------------
    engine_up();
    check(run("var a=input.text.open({" RECT ",ime:'off',initial:'x'});"
              "a.close(); a.close();"
              "try { a.getText(); note('STALE-READ(wrong)'); }"
              "catch(e){ note('STALE '+e.code); }"),"stale handle");
    check(logged("STALE CLOSED"),
          "a closed session answers CLOSED, not freed memory");
    engine_down();

}

static void case7(void) {
    // ---- 7. app teardown with a session up --------------------------------
    // app_stop() calls pocket_text_reset() while the guest is still alive; the
    // three callbacks it holds are freed against a context that still exists.
    engine_up();
    check(run("input.text.open({" RECT ",ime:'off',"
              "  onEdit:function(){}, onSubmit:function(){},"
              "  onCancel:function(){}});"),"opens with all three listeners");
    key("m");
    check(pocket_text_active(),"still up at teardown");
    engine_down();                                  // reset happens in here
    check(!pocket_text_active(),"reset closed it silently");
}

int main(int argc, char **argv) {
    setvbuf(stdout,NULL,_IONBF,0);      // the report must survive an abort
    host_ime_present(true);
    void (*cases[])(void)={case1,case2,case3,case4,case5,case6,case7};
    for(int i=0;i<(int)(sizeof cases/sizeof cases[0]);i++) {
        char want[2]={(char)('1'+i),'\0'};
        if(argc>1 && strcmp(argv[1],want)) continue;
        printf("-- case %d\n",i+1);
        cases[i]();
    }
    printf(failures?"\n%u FAILED\n":"\nall passed\n",failures);
    return failures?1:0;
}
