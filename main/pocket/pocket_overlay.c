#include "pocket_overlay.h"
#include "pocket_api.h"
#include "paint.h"
#include "jpfont.h"
#include "board.h"
#include <string.h>
#include <stdio.h>

// Raised from 16 when the overlay became the whole home screen (3.1, revised
// 2026-09-09): a status box in a corner needs a handful of items and a player
// that owns the panel needs rows. Each item is the struct below -- coordinates,
// a colour and OVERLAY_TEXT_MAX bytes -- so the cost is static and small, and
// it is charged to every build whether an overlay runs or not.
#define OVERLAY_ITEMS      24
// Bytes, plus the terminator. Raised from 23 with the region: the panel is 240
// px and the text face is 6x12 for ASCII and 12x12 for CJK, so 47 bytes is
// about one full width either way -- 47 latin characters or 15 Japanese ones.
// 23 was sized for a clock in a corner and cut a track name off the card in the
// middle of its second word.
#define OVERLAY_TEXT_MAX   47

typedef enum { ITEM_RECT=0, ITEM_TEXT } item_kind_t;

typedef struct {
    uint8_t  kind;
    int16_t  x,y,w,h;
    uint16_t colour;
    char     text[OVERLAY_TEXT_MAX+1];
} item_t;

// 16 * 36 bytes. The list is fixed rather than grown because the whole point of
// this surface is that an overlay cannot cause an allocation on the home
// screen: the scene's scratch is live, and the Rust core does not report
// running out -- it panics and reboots.
static item_t          items[OVERLAY_ITEMS];
static unsigned        count;
static overlay_region_t region;
static bool            have_region;
static bool            drawn;

// ------------------------------------------------------------------- keys
//
// The overlay's half of 3.1's input rule. The other half is main.c, which takes
// the shell's reserved key before anything reaches here; nothing in this file
// could restore it if that call site were wrong, and nothing here tries to --
// a second filter would only make the first one look optional.
#define OVERLAY_KEY_SUBS 2
#define OVERLAY_KEY_RING 8

typedef struct { char action[8]; char text[8]; uint8_t len; } key_event_t;
static key_event_t     keyq[OVERLAY_KEY_RING];
static unsigned        key_head, key_tail;
static pocket_sub_slot_t key_slots[OVERLAY_KEY_SUBS];
static pocket_sub_table_t key_table = {
    .slots=key_slots, .count=OVERLAY_KEY_SUBS, .tag="pocket.overlay",
    .what="onKey",
    // FALSE, unlike the polled surfaces. Their reason for closing a listener
    // that throws is runaway: one that throws every frame fills the log and
    // costs a call for ever. A key listener fires only when a person presses a
    // key, so there is nothing to run away -- and closing it costs the person
    // the keyboard. That happened: a transient out-of-memory inside the
    // listener took the player's input away permanently, and the failure was
    // the closing rather than the throw. capabilities.onChange makes the same
    // call for the same reason.
    .close_on_throw=false,
};

static const char *action_of(board_key_t nav) {
    switch(nav) {
        case KEY_UP:    return "up";
        case KEY_DOWN:  return "down";
        case KEY_LEFT:  return "left";
        case KEY_RIGHT: return "right";
        case KEY_ENTER: return "accept";
        // KEY_BACK is absent by construction: it is the shell's reserved key
        // and main.c returns before calling in. Listing it here as a case that
        // maps to nothing would suggest it can arrive.
        default:        return NULL;
    }
}

void pocket_overlay_key(const keystroke_t *k) {
    if(!k||!key_table.ctx) return;
    const char *action=action_of(k->nav);
    // A stroke that is neither an action nor text carries nothing a guest could
    // act on -- a modifier, or a key this keymap does not name.
    if(!action&&!k->len) return;
    key_event_t *e=&keyq[key_head%OVERLAY_KEY_RING];
    snprintf(e->action,sizeof e->action,"%s",action?action:"");
    e->len=k->len<sizeof e->text?k->len:(uint8_t)(sizeof e->text-1);
    memcpy(e->text,k->text,e->len);
    e->text[e->len]='\0';
    key_head++;
    if(key_head-key_tail>OVERLAY_KEY_RING) key_tail=key_head-OVERLAY_KEY_RING;
}

static bool key_payload(JSContext *ctx, int slot, void *user, JSValue *out) {
    (void)slot;
    const key_event_t *e=user;
    JSValue o=JS_NewObject(ctx);
    if(JS_IsException(o)) return false;
    JS_SetPropertyStr(ctx,o,"action",
                      e->action[0]?JS_NewString(ctx,e->action):JS_NULL);
    // A "\0name" token has its length in len and a leading NUL that strlen
    // would stop at, so the text is built from the bytes rather than from a C
    // string. keymap.h says why that NUL is load bearing.
    JS_SetPropertyStr(ctx,o,"key",
        e->len&&e->text[0]?JS_NewStringLen(ctx,e->text,e->len):JS_NULL);
    JS_SetPropertyStr(ctx,o,"phase",JS_NewString(ctx,"press"));
    *out=o;
    return true;
}

void pocket_overlay_pump(void) {
    while(key_tail!=key_head) {
        key_event_t e=keyq[key_tail%OVERLAY_KEY_RING];
        key_tail++;
        pocket_api_sub_deliver(&key_table,key_payload,&e);
    }
}

static JSValue js_on_key(JSContext *ctx, JSValueConst self, int argc,
                         JSValueConst *argv) {
    (void)self;
    key_table.ctx=ctx;
    return pocket_api_sub_open(ctx,&key_table,argc>0?argv[0]:JS_UNDEFINED,
                               "overlay.onKey","two key listeners are open",NULL);
}


void pocket_overlay_set_region(const overlay_region_t *r) {
    region=*r; have_region=true; count=0; drawn=false;
}
void pocket_overlay_reset(void) {
    pocket_api_sub_close_all(&key_table);
    key_table.ctx=NULL;
    key_head=key_tail=0;
    count=0; have_region=false; drawn=false;
    memset(&region,0,sizeof region);
}
bool pocket_overlay_drawn(void) { return drawn; }

void pocket_overlay_paint(uint16_t *strip, int strip_y, int strip_h) {
    if(!count) return;
    paint_begin(strip,strip_y,strip_h);
    // The region is clipped here as well as refused above. Not a contradiction:
    // the refusal is the contract with the app author, and this is the shell
    // refusing to trust its own arithmetic with the frame buffer.
    paint_clip(region.x,region.x+region.w);
    for(unsigned i=0;i<count;i++) {
        const item_t *it=&items[i];
        // Skip what this strip cannot contain. Measured 5.33 ms a frame before
        // this line: every item was handed to paint or to the font for every
        // one of the seventeen strips, and each call clipped it away again.
        // The clip inside those is still what makes the pixels right; this is
        // only about not asking seventeen times for an answer that is no
        // sixteen of them.
        int top=region.y+it->y;
        int bottom=top+(it->kind==ITEM_RECT?it->h:12);
        if(bottom<=strip_y||top>=strip_y+strip_h) continue;
        if(it->kind==ITEM_RECT) paint_fill(region.x+it->x,region.y+it->y,
                                           it->w,it->h,it->colour);
        else {
            // Through the Japanese face when it is up, exactly as the pickers
            // do it. paint_ascii is the 5x7 latin face and draws anything
            // outside 0x20-0x7E as '?', so an overlay showing a track name off
            // the card printed a row of question marks -- and the app could not
            // tell, because the bytes it sent were correct. The fallback is
            // kept for the window before the atlas exists.
            size_t n=strlen(it->text);
            if(jpfont_ready(JPFONT_TEXT))
                jpfont_draw(JPFONT_TEXT,strip,strip_y,strip_h,
                            region.x+it->x,region.y+it->y,it->text,n,it->colour);
            else
                paint_ascii(region.x+it->x,region.y+it->y,it->text,it->colour);
        }
    }
}

// ---------------------------------------------------------------- JS surface

static JSValue outside(JSContext *ctx, const char *op) {
    return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                            "outside the overlay's region",false,
                            POCKET_OUTCOME_NOT_APPLIED);
}
static bool colour_of(JSContext *ctx, int argc, JSValueConst *argv, int first,
                      uint16_t *out) {
    int32_t rgb[3]={255,255,255};
    for(int i=0;i<3;i++) {
        if(argc<=first+i) break;
        if(JS_ToInt32(ctx,&rgb[i],argv[first+i])) return false;
        if(rgb[i]<0) rgb[i]=0; else if(rgb[i]>255) rgb[i]=255;
    }
    *out=board_rgb((unsigned)rgb[0],(unsigned)rgb[1],(unsigned)rgb[2]);
    return true;
}
static item_t *take(JSContext *ctx, const char *op) {
    if(!have_region) {
        pocket_api_throw(ctx,POCKET_ERR_NOT_AVAILABLE,op,
                         "this session is not an overlay",false,
                         POCKET_OUTCOME_NOT_APPLIED);
        return NULL;
    }
    if(count>=OVERLAY_ITEMS) {
        pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,
                         "the overlay display list is full",false,
                         POCKET_OUTCOME_NOT_APPLIED);
        return NULL;
    }
    return &items[count];
}

// begin() rather than an implicit clear at the top of every frame: an overlay
// that draws nothing this turn keeps what it drew last turn, and that has to be
// something it says rather than something the host guesses.
static JSValue js_begin(JSContext *ctx, JSValueConst self, int argc,
                        JSValueConst *argv) {
    (void)ctx;(void)self;(void)argc;(void)argv;
    count=0;
    return JS_UNDEFINED;
}

static JSValue js_rect(JSContext *ctx, JSValueConst self, int argc,
                       JSValueConst *argv) {
    (void)self;
    static const char *const OP="overlay.rect";
    int32_t x,y,w,h;
    if(argc<4) return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                       "rect(x, y, w, h, r, g, b)",false,
                                       POCKET_OUTCOME_NOT_APPLIED);
    if(JS_ToInt32(ctx,&x,argv[0])||JS_ToInt32(ctx,&y,argv[1])||
       JS_ToInt32(ctx,&w,argv[2])||JS_ToInt32(ctx,&h,argv[3])) return JS_EXCEPTION;
    if(!overlay_region_holds(&region,x,y,w,h)) return outside(ctx,OP);
    item_t *it=take(ctx,OP); if(!it) return JS_EXCEPTION;
    uint16_t colour;
    if(!colour_of(ctx,argc,argv,4,&colour)) return JS_EXCEPTION;
    *it=(item_t){.kind=ITEM_RECT,.x=(int16_t)x,.y=(int16_t)y,
                 .w=(int16_t)w,.h=(int16_t)h,.colour=colour};
    count++; drawn=true;
    return JS_UNDEFINED;
}

static JSValue js_text(JSContext *ctx, JSValueConst self, int argc,
                       JSValueConst *argv) {
    (void)self;
    static const char *const OP="overlay.text";
    int32_t x,y;
    if(argc<3) return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                       "text(x, y, string, r, g, b)",false,
                                       POCKET_OUTCOME_NOT_APPLIED);
    if(JS_ToInt32(ctx,&x,argv[0])||JS_ToInt32(ctx,&y,argv[1])) return JS_EXCEPTION;
    size_t length=0;
    const char *s=JS_ToCStringLen(ctx,&length,argv[2]);
    if(!s) return JS_EXCEPTION;
    if(length>OVERLAY_TEXT_MAX) {
        JS_FreeCString(ctx,s);
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                "overlay text is limited to 47 bytes",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    }
    // The box the text will occupy, MEASURED THE WAY IT IS DRAWN. This said
    // `length*6-1` by 7 while paint_ascii was doing the drawing, and stayed
    // that way for one build after the drawing moved to the 12 px face -- so a
    // line of Japanese was measured at three times its width and a line of
    // latin was measured at half its height. Both errors are silent: one
    // refuses a string that fits, the other admits one that does not.
    //
    // Counted here rather than through jpfont_width() because that answer
    // depends on whether the atlas is loaded, and a contract that moves when a
    // font finishes loading is not a contract. shinonome is 6x12 for ASCII and
    // 12x12 for everything else, and a continuation byte adds nothing.
    int w=0;
    for(size_t i=0;i<length;i++) {
        unsigned char c=(unsigned char)s[i];
        if(c<0x80) w+=6;
        else if((c&0xC0)!=0x80) w+=12;
    }
    int h=12;
    if(!w) w=1; else w-=1;
    if(!overlay_region_holds(&region,x,y,w,h)) { JS_FreeCString(ctx,s);
                                                 return outside(ctx,OP); }
    item_t *it=take(ctx,OP);
    if(!it) { JS_FreeCString(ctx,s); return JS_EXCEPTION; }
    uint16_t colour;
    if(!colour_of(ctx,argc,argv,3,&colour)) { JS_FreeCString(ctx,s); return JS_EXCEPTION; }
    *it=(item_t){.kind=ITEM_TEXT,.x=(int16_t)x,.y=(int16_t)y,.colour=colour};
    memcpy(it->text,s,length); it->text[length]='\0';
    JS_FreeCString(ctx,s);
    count++; drawn=true;
    return JS_UNDEFINED;
}

// ------------------------------------------------------------- capability

static pocket_limit_t overlay_limits[] = {
    {"maxItems",     POCKET_LIMIT_INT, OVERLAY_ITEMS,    NULL},
    {"maxTextChars", POCKET_LIMIT_INT, OVERLAY_TEXT_MAX, NULL},
    {"keyListeners", POCKET_LIMIT_INT, OVERLAY_KEY_SUBS,  NULL},
    // What an overlay can never be sent. Named in limits rather than left to be
    // discovered, because an app that waited for it would wait for ever.
    {"reservedKeys", POCKET_LIMIT_TEXT, 0, "back"},
    {"regionWidth",  POCKET_LIMIT_INT, 0,                NULL},
    {"regionHeight", POCKET_LIMIT_INT, 0,                NULL},
    {NULL, POCKET_LIMIT_END, 0, NULL},
};
// available follows the region: outside an overlay session there is nowhere to
// draw, and section 2 wants available to be an observation of now.
static void probe(const pocket_capability_t *cap, bool *available,
                  const char **reason) {
    (void)cap;
    *available=have_region;
    if(!have_region) *reason=POCKET_REASON_DISABLED;
}
static const pocket_capability_t overlay_capability = {
    .name="ui.overlay", .supported=true, .available=false,
    .reason=POCKET_REASON_DISABLED, .limits=overlay_limits, .probe=probe,
};

static esp_err_t build_overlay(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    JS_SetPropertyStr(ctx,ns,"begin",JS_NewCFunction(ctx,js_begin,"begin",0));
    JS_SetPropertyStr(ctx,ns,"rect",JS_NewCFunction(ctx,js_rect,"rect",7));
    JS_SetPropertyStr(ctx,ns,"text",JS_NewCFunction(ctx,js_text,"text",6));
    JS_SetPropertyStr(ctx,ns,"onKey",JS_NewCFunction(ctx,js_on_key,"onKey",1));
    JSValue box=JS_NewObject(ctx);
    if(JS_IsException(box)) return ESP_ERR_NO_MEM;
    JS_SetPropertyStr(ctx,box,"width",JS_NewInt32(ctx,region.w));
    JS_SetPropertyStr(ctx,box,"height",JS_NewInt32(ctx,region.h));
    JS_SetPropertyStr(ctx,ns,"region",box);
    return ESP_OK;
}

esp_err_t pocket_overlay_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    // By NAME, not by index. These were overlay_limits[2] and [3] until two
    // rows were added above them, and an index here would have kept compiling
    // while reporting the wrong two numbers -- the failure this file is least
    // able to notice, since a limit nobody enforces cannot disagree with
    // anything.
    for(pocket_limit_t *l=overlay_limits;l->kind!=POCKET_LIMIT_END;l++) {
        if(!strcmp(l->name,"regionWidth"))  l->number=region.w;
        if(!strcmp(l->name,"regionHeight")) l->number=region.h;
    }
    pocket_api_register(&overlay_capability);
    return pocket_api_lazy(ctx,"overlay",build_overlay,NULL);
}
