#include "pocket_text.h"
#include "pocket_api.h"
#include "app_session.h"
#include "textfield.h"
#include "board.h"
#include "paint.h"
#include "jpfont.h"
#include "skk_session.h"
#include "skk_core.h"
#include "esp_log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG="pocket.text";

// A SMALL body-text field, and the size is the boundary section 6 draws: "初版は
// 高水準の本文入力欄まで。Playgroundの大きなコード編集バッファを毎打鍵JSへ全コピー
// する設計にはしない". 256 bytes is about 85 Japanese characters -- a name, a
// note, a search term -- and it is also what keeps getText() cheap enough that
// an app may call it from onEdit without anybody having to warn it not to.
// The code editor keeps its own 8 KB buffer and is not built on this.
#define TEXT_MAX_BYTES     256
#define TEXT_DEFAULT_BYTES 128
#define TEXT_MIN_W  24
#define TEXT_MIN_H  14

// One session at a time, so this is a pointer and not a table.
typedef struct {
    textfield_t field;
    JSContext  *ctx;
    JSValue     on_edit, on_submit, on_cancel;
    int         x,y,w,h;
    bool        ime;              // TextOptions.ime === "on" and a dictionary
    uint32_t    handle;           // matched against the JS object's opaque
    char        buf[];            // maxBytes+1, allocated with the session
} session_t;

static session_t *live;
static bool       dirty;
static JSClassID  session_class;
static bool       class_ready;
// Never reset per session: a handle identifies a session for the life of the
// firmware run, so a TextSession object the app kept from a closed session can
// never be mistaken for the one that replaced it. Same argument as
// pocket_api.c's request numbers, and the same instruction not to "tidy" it.
static uint32_t   handle_seed=1;

bool pocket_text_active(void) { return live!=NULL; }
bool pocket_text_take_dirty(void) { bool was=dirty; dirty=false; return was; }

// ------------------------------------------------------------------- the IME

static bool ime_available(void) { return skk_session_ready(); }

// Focus moved. Two things follow from that and they are both here so neither
// can be forgotten: any reading the engine is holding is DISCARDED rather than
// committed (ime_core's own rule for losing focus -- a mis-timed commit that
// lands in the wrong field is worse than a few characters retyped), and the
// generation is bumped so a commit already computed for the old field is
// refused by tf_commit().
static void refocus(session_t *s, bool arm) {
    if(ime_available()) {
        ime_reset(skk_session());
        ime_set_on(skk_session(),arm && s && s->ime);
    }
    if(s) tf_refocus(&s->field);
}

// --------------------------------------------------------------- the session

// Takes the session off the module before anything else runs, so a callback
// that opens a new one -- or closes this one again -- finds a consistent world.
static session_t *detach(void) {
    session_t *s=live;
    live=NULL;
    if(s) { refocus(s,false); dirty=true; }
    return s;
}

static void destroy(session_t *s) {
    if(!s) return;
    JSContext *ctx=s->ctx;
    if(ctx) {
        JS_FreeValue(ctx,s->on_edit);
        JS_FreeValue(ctx,s->on_submit);
        JS_FreeValue(ctx,s->on_cancel);
    }
    free(s);
}

void pocket_text_reset(void) {
    session_t *s=detach();
    if(s) ESP_LOGI(TAG,"TEXT_RESET %u bytes",(unsigned)s->field.len);
    destroy(s);
    dirty=false;
    // The class belongs to the realm that is going away.
    class_ready=false;
}

// Calls one listener and reports whether the session survived it. `s` may be
// freed by the call -- an onEdit that calls close() does exactly that -- so
// nothing after this may touch it unless the answer is true.
static bool fire(session_t *s, JSValueConst fn, int argc, JSValueConst *argv) {
    // Everything this function needs after the call is read BEFORE it. `s` is
    // one calloc of session+buffer, so a close() from inside the listener frees
    // the whole block -- and the reopen that usually follows takes the same
    // size class straight back off the free list, which is what turns `s->ctx`
    // after the call from a poisoned read into a plausible-looking pointer
    // built out of the NEW session's field. Cache, do not reload.
    JSContext *ctx=s->ctx;
    if(!ctx || !JS_IsFunction(ctx,fn)) return live==s;
    uint32_t handle=s->handle;
    // The listener is called through a reference of OUR own, and this is the
    // one that stops the board rebooting. `fn` is borrowed from s->on_*, and
    // the session holds the only reference to it: a listener that calls
    // close() reaches destroy(), which frees exactly those three JSValues --
    // so the function object, its bytecode and the atoms of every string
    // literal after the close() are released WHILE THAT FUNCTION IS STILL
    // RUNNING. On the board that is the LoadProhibited: `c.close(); reopen();`
    // dies on the second statement, which is why OPEN-D was never printed.
    // QuickJS's own rule -- the caller keeps the callee alive for the call --
    // was the one being broken here, and it cannot be fixed by handle checks
    // afterwards because the crash happens before "afterwards".
    JSValue held=JS_DupValue(ctx,fn);
    JSValue result=JS_Call(ctx,held,JS_UNDEFINED,argc,argv);
    JS_FreeValue(ctx,held);
    if(JS_IsException(result)) {
        JSValue e=JS_GetException(ctx);
        const char *text=JS_ToCString(ctx,e);
        ESP_LOGW(TAG,"listener threw: %s",text?text:"?");
        if(text) JS_FreeCString(ctx,text);
        JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,result);
    return live && live->handle==handle;
}

// onEdit carries the COMMITTED text and nothing else. A preedit never reaches
// it, because a preedit never reaches the buffer: ime_feed() returns IME_TEXT
// only for what the engine has settled, and that is the one string this file
// ever copies out of the IME.
static bool fire_edit(session_t *s) {
    // Same rule as fire(): the context is taken before the listener runs,
    // because the event still has to be released after a listener that closed
    // the session out from under us.
    JSContext *ctx=s->ctx;
    if(!ctx) return true;
    JSValue event=JS_NewObject(ctx);
    if(JS_IsException(event)) { JS_FreeValue(ctx,event); return live==s; }
    JS_SetPropertyStr(ctx,event,"text",
                      JS_NewStringLen(ctx,s->field.buf,s->field.len));
    JSValueConst argv[1]={event};
    bool alive=fire(s,s->on_edit,1,argv);
    JS_FreeValue(ctx,event);
    return alive;
}

// Both endings close the session automatically, and both do it BEFORE the
// callback runs: section 6 says onSubmit/onCancelで自動closeし, and an app that
// opens a second field from inside onSubmit must not be told the first one is
// still up. The text is copied out first for the same reason.
static void finish_submit(session_t *s) {
    JSContext *ctx=s->ctx;
    JSValue event=JS_UNDEFINED;
    if(ctx) {
        event=JS_NewObject(ctx);
        if(!JS_IsException(event))
            JS_SetPropertyStr(ctx,event,"text",
                              JS_NewStringLen(ctx,s->field.buf,s->field.len));
    }
    ESP_LOGI(TAG,"TEXT_SUBMIT %u bytes",(unsigned)s->field.len);
    detach();
    JSValueConst argv[1]={event};
    fire(s,s->on_submit,JS_IsUndefined(event)?0:1,argv);
    if(ctx) JS_FreeValue(ctx,event);
    destroy(s);
}

static void finish_cancel(session_t *s) {
    ESP_LOGI(TAG,"TEXT_CANCEL");
    detach();
    fire(s,s->on_cancel,0,NULL);
    destroy(s);
}

// ------------------------------------------------------------ the key path

void pocket_text_key(const keystroke_t *k) {
    session_t *s=live;
    if(!s || !k) return;
    dirty=true;

    // Ctrl+J / opt+Space carries no text, so it would fall through the length
    // test below. It is the same toggle every other text screen here has.
    if(k->toggle_ime) {
        if(ime_available() && s->ime) {
            ime_t *im=skk_session();
            ime_set_on(im,!ime_on(im));
            ESP_LOGI(TAG,"TEXT_IME %s",ime_on(im)?"ON":"OFF");
        }
        return;
    }
    if(!k->len) return;

    // The engine first, always -- ime_core makes that ordering structural, and
    // it is what decides the two keys section 6 singles out. An Enter that
    // commits a conversion comes back IME_TEXT and an Escape that cancels one
    // comes back IME_TAKEN, so neither reaches tf_key() and neither can be
    // delivered twice.
    bool took=false;
    if(s->ime && ime_available()) {
        // Read BEFORE the feed: this is the field the commit is being computed
        // for, and tf_commit() refuses to write it anywhere else.
        uint32_t generation=s->field.generation;
        ime_t *im=skk_session();
        ime_disp_t d=ime_feed(im,k->text,k->len);
        if(d==IME_TEXT) {
            size_t len=0;
            const char *text=ime_text(im,&len);
            if(!tf_commit(&s->field,generation,text,len))
                ESP_LOGW(TAG,"TEXT_STALE %u bytes dropped (gen %u != %u)",
                         (unsigned)len,(unsigned)generation,
                         (unsigned)s->field.generation);
            else if(!fire_edit(s)) return;   // a listener closed or replaced it
            took=true;
        } else took=(d==IME_TAKEN);
    }

    switch(tf_key(&s->field,k->text,k->len,took)) {
        case TF_EDIT:   fire_edit(s); break;
        case TF_SUBMIT: finish_submit(s); break;
        case TF_CANCEL: finish_cancel(s); break;
        case TF_NONE:   break;
    }
}

// ---------------------------------------------------------------- the overlay
//
// Drawn into the guest's own strips, after the renderer has filled them: this
// is "ホスト所有の小さな編集欄を画面へ合成する" and it is the same seam
// pet_assets_overlay() uses, for the same reason -- one strip buffer exists on
// this board and the frame is already in it.

// How far the text is scrolled left so the cursor stays inside the box. Kept
// across frames because it must not jitter while a wide character is typed.
static int scroll_px;

static int glyph_advance(const char *s, size_t len, size_t i, size_t *adv) {
    if(jpfont_ready(JPFONT_TEXT))
        return (int)jpfont_advance(JPFONT_TEXT,s,len,i,adv);
    *adv=1;                                 // the 5x7 face, one cell per byte
    return 6;
}

static int text_width(const char *s, size_t len) {
    int w=0;
    for(size_t i=0;i<len;) {
        size_t adv=0;
        w+=glyph_advance(s,len,i,&adv);
        if(!adv) break;
        i+=adv;
    }
    return w;
}

// One run, glyph by glyph, drawing only the characters wholly inside
// [left,right). jpfont_draw() has no clip box and the core's own scissor does
// not reach here, so without this a name longer than the field paints across
// the app's display -- section 6 asks a text box to clip, and this is the only
// place on this host where that is cheap enough to actually do.
static int draw_clipped(uint16_t *pixels, int y, int rows, int pen, int ty,
                        const char *s, size_t len, uint16_t colour,
                        int left, int right) {
    for(size_t i=0;i<len;) {
        size_t adv=0;
        int w=glyph_advance(s,len,i,&adv);
        if(!adv) break;
        if(pen>=left && pen+w<=right) {
            if(jpfont_ready(JPFONT_TEXT))
                jpfont_draw(JPFONT_TEXT,pixels,y,rows,pen,ty,s+i,adv,colour);
            else {
                char one[2]={s[i],0};
                paint_ascii(pen,ty+2,one,colour);
            }
        }
        pen+=w; i+=adv;
    }
    return pen;
}

void pocket_text_overlay(uint16_t *pixels, int y, int rows) {
    session_t *s=live;
    if(!s) return;
    // Nothing of the box is in this strip: the common case, and it costs one
    // compare per strip on the fourteen of seventeen a session is not on.
    if(y+rows<=s->y || y>=s->y+s->h) return;
    paint_begin(pixels,y,rows);

    const uint16_t back  =board_rgb(10,16,26);
    const uint16_t rule  =board_rgb(90,120,158);
    const uint16_t ink   =board_rgb(228,236,246);
    const uint16_t accent=board_rgb(120,200,255);
    const uint16_t caret =board_rgb(255,210,120);

    paint_fill(s->x,s->y,s->w,s->h,back);
    paint_fill(s->x,s->y,s->w,1,rule);
    paint_fill(s->x,s->y+s->h-1,s->w,1,rule);
    paint_fill(s->x,s->y,1,s->h,rule);
    paint_fill(s->x+s->w-1,s->y,1,s->h,rule);

    size_t plen=0;
    const char *pre = (s->ime && ime_available())
                      ? ime_preedit(skk_session(),&plen) : NULL;

    int left=s->x+2, right=s->x+s->w-2;
    int inner=right-left;
    // Where the caret sits in the run, which is after the preedit when the IME
    // is holding one -- the preedit is drawn AT the cursor.
    int want=text_width(s->field.buf,s->field.cursor)
             +(pre&&plen?text_width(pre,plen):0);
    if(want-scroll_px>inner-2) scroll_px=want-inner+2;
    if(want-scroll_px<0)       scroll_px=want;
    if(scroll_px<0)            scroll_px=0;

    int pen=left+1-scroll_px;
    int ty=s->y+(s->h-12)/2;
    // Three runs: the text before the cursor, the preedit in its own colour,
    // and the text after. The preedit is DRAWN and never stored, which is the
    // whole of "IME未確定文字はgetText/onEditへ含めず".
    pen=draw_clipped(pixels,y,rows,pen,ty,s->field.buf,s->field.cursor,ink,
                     left,right);
    if(pre&&plen)
        pen=draw_clipped(pixels,y,rows,pen,ty,pre,plen,accent,left,right);
    int caret_x=pen;
    draw_clipped(pixels,y,rows,pen,ty,s->field.buf+s->field.cursor,
                 s->field.len-s->field.cursor,ink,left,right);
    if(caret_x>=left && caret_x<right)
        paint_fill(caret_x,s->y+2,1,s->h-4,caret);
}

// ------------------------------------------------------------------ the JS

static JSValue bad(JSContext *ctx, const char *what) {
    return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"input.text.open",
                            what,false,NULL);
}

// The session behind `this`, or NULL when the object outlived it. The opaque is
// the handle rather than the pointer, so a TextSession the app kept after its
// session closed answers CLOSED instead of reading freed memory.
static session_t *this_session(JSValueConst self) {
    uint32_t handle=(uint32_t)(uintptr_t)JS_GetOpaque(self,session_class);
    if(!handle || !live || live->handle!=handle) return NULL;
    return live;
}

static JSValue js_get_text(JSContext *ctx, JSValueConst self,
                           int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    session_t *s=this_session(self);
    if(!s) return pocket_api_throw(ctx,POCKET_ERR_CLOSED,"input.text.getText",
                                   "this text session is closed",false,NULL);
    // The committed text. Whatever the IME is holding is not part of it.
    return JS_NewStringLen(ctx,s->field.buf,s->field.len);
}

// Idempotent, and fires nothing: the app asked, so there is nothing to tell it.
static JSValue js_close(JSContext *ctx, JSValueConst self,
                        int argc, JSValueConst *argv) {
    (void)ctx; (void)argc; (void)argv;
    session_t *s=this_session(self);
    if(!s) return JS_UNDEFINED;
    ESP_LOGI(TAG,"TEXT_CLOSE %u bytes",(unsigned)s->field.len);
    detach();
    destroy(s);
    return JS_UNDEFINED;
}

static const JSCFunctionListEntry session_methods[] = {
    JS_CFUNC_DEF("getText",0,js_get_text),
    JS_CFUNC_DEF("close",0,js_close),
};
static const JSClassDef session_def = {.class_name="PocketTextSession"};

// No finalizer, for pocket_ui.c's reason: the session is owned by the host, not
// by the wrapper the app happens to be holding. An app that drops the object
// still has a field on screen, and Escape still closes it.
static bool ensure_class(JSContext *ctx) {
    if(class_ready) return true;
    JSRuntime *rt=JS_GetRuntime(ctx);
    JS_NewClassID(rt,&session_class);
    if(JS_NewClass(rt,session_class,&session_def)<0) return false;
    JSValue proto=JS_NewObject(ctx);
    if(JS_IsException(proto)) return false;
    JS_SetPropertyFunctionList(ctx,proto,session_methods,
                               (int)(sizeof(session_methods)/sizeof(session_methods[0])));
    JS_SetClassProto(ctx,session_class,proto);
    class_ready=true;
    return true;
}

// One optional listener out of TextOptions. Absent is fine; present and not a
// function is not, because an app that misspells onSubmit would otherwise get a
// field it can never finish.
static bool take_listener(JSContext *ctx, JSValueConst options, const char *name,
                          JSValue *out) {
    *out=JS_UNDEFINED;
    JSValue fn=JS_GetPropertyStr(ctx,options,name);
    if(JS_IsException(fn)) return false;
    if(JS_IsUndefined(fn)||JS_IsNull(fn)) { JS_FreeValue(ctx,fn); return true; }
    if(!JS_IsFunction(ctx,fn)) {
        JS_FreeValue(ctx,fn);
        char note[48];
        snprintf(note,sizeof(note),"%s must be a function",name);
        bad(ctx,note);
        return false;
    }
    *out=fn;
    return true;
}

static bool take_int(JSContext *ctx, JSValueConst spec, const char *name,
                     double fallback, double lo, double hi, double *out) {
    JSValue value=JS_GetPropertyStr(ctx,spec,name);
    if(JS_IsException(value)) return false;
    if(JS_IsUndefined(value)||JS_IsNull(value)) {
        JS_FreeValue(ctx,value); *out=fallback; return true;
    }
    int failed=JS_ToFloat64(ctx,out,value);
    JS_FreeValue(ctx,value);
    if(failed) return false;
    if(!isfinite(*out) || *out<lo || *out>hi) {
        char note[64];
        snprintf(note,sizeof(note),"%s is outside %d..%d",name,(int)lo,(int)hi);
        bad(ctx,note);
        return false;
    }
    return true;
}

static JSValue js_open(JSContext *ctx, JSValueConst self,
                       int argc, JSValueConst *argv) {
    (void)self;
    if(argc<1 || !JS_IsObject(argv[0]))
        return bad(ctx,"open(options) takes an object");
    JSValueConst options=argv[0];

    // 同時に1セッション. BUSY rather than replacing the open one: the field on
    // screen belongs to whoever is typing into it, and taking it away mid-word
    // is not the host's to do.
    if(live)
        return pocket_api_throw(ctx,POCKET_ERR_BUSY,"input.text.open",
                                "a text session is already open",true,
                                POCKET_OUTCOME_NOT_APPLIED);
    if(!ensure_class(ctx))
        return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,"input.text.open",
                                "no room for the session class",true,
                                POCKET_OUTCOME_NOT_APPLIED);

    JSValue rect=JS_GetPropertyStr(ctx,options,"rect");
    if(JS_IsException(rect)) return JS_EXCEPTION;
    if(!JS_IsObject(rect)) {
        JS_FreeValue(ctx,rect);
        return bad(ctx,"rect {x,y,width,height} is required");
    }
    double x,y,w,h;
    bool geometry = take_int(ctx,rect,"x",NAN,0,LCD_W-TEXT_MIN_W,&x) &&
                    take_int(ctx,rect,"y",NAN,0,LCD_H-TEXT_MIN_H,&y) &&
                    take_int(ctx,rect,"width",NAN,TEXT_MIN_W,LCD_W,&w) &&
                    take_int(ctx,rect,"height",NAN,TEXT_MIN_H,LCD_H,&h);
    JS_FreeValue(ctx,rect);
    if(!geometry) return JS_EXCEPTION;
    if(isnan(x)||isnan(y)||isnan(w)||isnan(h))
        return bad(ctx,"rect needs x, y, width and height");
    if(x+w>LCD_W || y+h>LCD_H)
        return bad(ctx,"rect must fit on the 240x135 display");

    double max_bytes;
    if(!take_int(ctx,options,"maxBytes",TEXT_DEFAULT_BYTES,1,TEXT_MAX_BYTES,
                 &max_bytes)) return JS_EXCEPTION;

    JSValue multi=JS_GetPropertyStr(ctx,options,"multiline");
    if(JS_IsException(multi)) return JS_EXCEPTION;
    bool multiline=JS_ToBool(ctx,multi)!=0;
    JS_FreeValue(ctx,multi);

    // ime is "off" or "on", and anything else is refused rather than read as
    // one of them: a later mode must not silently mean this one on a firmware
    // that predates it. Same rule workspace.pick applies to `kind`.
    bool want_ime=false;
    JSValue ime=JS_GetPropertyStr(ctx,options,"ime");
    if(JS_IsException(ime)) return JS_EXCEPTION;
    if(!JS_IsUndefined(ime) && !JS_IsNull(ime)) {
        const char *name=JS_ToCString(ctx,ime);
        bool ok=name && (!strcmp(name,"off")||!strcmp(name,"on"));
        if(ok) want_ime=!strcmp(name,"on");
        if(name) JS_FreeCString(ctx,name);
        JS_FreeValue(ctx,ime);
        if(!ok) return bad(ctx,"ime must be \"off\" or \"on\"");
    } else JS_FreeValue(ctx,ime);

    JSValue initial=JS_GetPropertyStr(ctx,options,"initial");
    if(JS_IsException(initial)) return JS_EXCEPTION;
    size_t initial_len=0;
    const char *initial_text=NULL;
    if(!JS_IsUndefined(initial) && !JS_IsNull(initial)) {
        if(!JS_IsString(initial)) {
            JS_FreeValue(ctx,initial);
            return bad(ctx,"initial must be a string");
        }
        initial_text=JS_ToCStringLen(ctx,&initial_len,initial);
        if(!initial_text) { JS_FreeValue(ctx,initial); return JS_EXCEPTION; }
    }

    JSValue on_edit,on_submit,on_cancel;
    bool listeners = take_listener(ctx,options,"onEdit",&on_edit) &&
                     take_listener(ctx,options,"onSubmit",&on_submit) &&
                     take_listener(ctx,options,"onCancel",&on_cancel);
    if(!listeners) {
        if(initial_text) JS_FreeCString(ctx,initial_text);
        JS_FreeValue(ctx,initial);
        // take_listener frees what it took before it fails, and the ones it
        // never reached are still JS_UNDEFINED.
        JS_FreeValue(ctx,on_edit); JS_FreeValue(ctx,on_submit);
        return JS_EXCEPTION;
    }

    // The buffer lives with the session, not in .bss: static DRAM is the
    // binding constraint of this firmware and a field nobody has opened must
    // cost nothing. Same argument codeedit.c was moved to the heap for.
    size_t cap=(size_t)max_bytes;
    session_t *s=calloc(1,sizeof(*s)+cap+1);
    if(!s) {
        if(initial_text) JS_FreeCString(ctx,initial_text);
        JS_FreeValue(ctx,initial);
        JS_FreeValue(ctx,on_edit); JS_FreeValue(ctx,on_submit);
        JS_FreeValue(ctx,on_cancel);
        return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,"input.text.open",
                                "no memory for the text session",true,
                                POCKET_OUTCOME_NOT_APPLIED);
    }
    tf_init(&s->field,s->buf,cap,multiline);
    bool ok = !initial_text || tf_set(&s->field,initial_text,initial_len);
    if(initial_text) JS_FreeCString(ctx,initial_text);
    JS_FreeValue(ctx,initial);
    if(!ok) {
        free(s);
        JS_FreeValue(ctx,on_edit); JS_FreeValue(ctx,on_submit);
        JS_FreeValue(ctx,on_cancel);
        return bad(ctx,"initial must be UTF-8 and no longer than maxBytes");
    }
    s->ctx=ctx;
    s->on_edit=on_edit; s->on_submit=on_submit; s->on_cancel=on_cancel;
    s->x=(int)x; s->y=(int)y; s->w=(int)w; s->h=(int)h;
    // ime:"on" without a dictionary or without the Japanese face is honoured as
    // far as it can be: the field takes latin as it always would, and nothing
    // pretends a conversion is available. It is not an error -- section 2's
    // available is an observation, and the app asked for a field, not for SKK.
    s->ime=want_ime && ime_available() && jpfont_ready(JPFONT_TEXT);
    s->handle=handle_seed++;
    if(!s->handle) s->handle=handle_seed++;   // never 0: that is "no session"

    JSValue object=JS_NewObjectClass(ctx,(int)session_class);
    if(JS_IsException(object)) {
        s->ctx=NULL;                  // the listeners are freed by destroy()
        JSContext *c=ctx;
        JS_FreeValue(c,s->on_edit); JS_FreeValue(c,s->on_submit);
        JS_FreeValue(c,s->on_cancel);
        free(s);
        return object;
    }
    JS_SetOpaque(object,(void *)(uintptr_t)s->handle);

    live=s;
    scroll_px=0;
    dirty=true;
    refocus(s,true);
    ESP_LOGI(TAG,"TEXT_OPEN %dx%d at %d,%d max=%u %s ime=%s",
             s->w,s->h,s->x,s->y,(unsigned)cap,
             multiline?"multiline":"single",s->ime?"on":"off");
    return object;
}

// ------------------------------------------------------------- capabilities

// Only what this file enforces. maxBytes is checked in js_open and again by
// tf_insert on every keystroke; maxSessions is the `live` pointer; ime says
// whether a conversion engine is actually behind the "on" option on this boot,
// which is the one number here that can differ between two identical builds.
static const pocket_limit_t text_limits[] = {
    {.name="maxBytes",    .kind=POCKET_LIMIT_INT,  .number=TEXT_MAX_BYTES},
    {.name="maxSessions", .kind=POCKET_LIMIT_INT,  .number=1},
    {.name="multiline",   .kind=POCKET_LIMIT_FLAG, .number=1},
    {.name="ime",         .kind=POCKET_LIMIT_TEXT, .text="skk"},
    {0},
};

// supported never moves -- the surface is in this firmware whether or not a
// dictionary was flashed, and a field still edits latin without one. What moves
// is available, and only for the reason open() actually refuses: 同時に1セッション.
// An app that asks while its own field is up is told BUSY here and by open().
static void text_probe(const pocket_capability_t *cap, bool *available,
                       const char **reason) {
    (void)cap;
    if(live) { *available=false; *reason=POCKET_REASON_BUSY; return; }
    *available=true; *reason=NULL;
}

static const pocket_capability_t text_capability = {
    .name="input.text", .supported=true, .available=true,
    .limits=text_limits, .probe=text_probe,
};

// ------------------------------------------------------------------ install

// A second contributor to `input`; pocket_ui.c registered the first, and
// contributors run in the order they registered. Neither file has to know what
// the other put on the namespace.
static esp_err_t build_text(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    if(!ensure_class(ctx)) return ESP_ERR_NO_MEM;
    JSValue text=JS_NewObject(ctx);
    if(JS_IsException(text)) return ESP_ERR_NO_MEM;
    JS_DefinePropertyValueStr(ctx,text,"open",
        JS_NewCFunction(ctx,js_open,"open",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"text",text,JS_PROP_ENUMERABLE);
    return ESP_OK;
}

esp_err_t pocket_text_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    pocket_api_register(&text_capability);
    // A previous session's realm is gone; nothing of it may be reused.
    live=NULL; dirty=false; class_ready=false; scroll_px=0;
    return pocket_api_lazy(ctx,"input",build_text,NULL);
}
