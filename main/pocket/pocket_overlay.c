#include "pocket_overlay.h"
#include "pocket_api.h"
#include "paint.h"
#include "board.h"
#include <string.h>

#define OVERLAY_ITEMS      16
#define OVERLAY_TEXT_MAX   23      // plus the terminator

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

void pocket_overlay_set_region(const overlay_region_t *r) {
    region=*r; have_region=true; count=0; drawn=false;
}
void pocket_overlay_reset(void) {
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
        if(it->kind==ITEM_RECT) paint_fill(region.x+it->x,region.y+it->y,
                                           it->w,it->h,it->colour);
        else                    paint_ascii(region.x+it->x,region.y+it->y,
                                            it->text,it->colour);
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
                                "overlay text is limited to 23 characters",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    }
    // The box the 5x7 face will occupy, measured the same way paint_ascii
    // draws it. Refusing here is what makes "the region is the whole of what
    // an overlay can reach" true of text as well as of boxes -- a string one
    // character too long for the box is a mistake its author should be told
    // about, and a clipped last letter is not being told.
    int w=(int)length*6-1, h=7;
    if(length==0) w=1;
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
    JSValue box=JS_NewObject(ctx);
    if(JS_IsException(box)) return ESP_ERR_NO_MEM;
    JS_SetPropertyStr(ctx,box,"width",JS_NewInt32(ctx,region.w));
    JS_SetPropertyStr(ctx,box,"height",JS_NewInt32(ctx,region.h));
    JS_SetPropertyStr(ctx,ns,"region",box);
    return ESP_OK;
}

esp_err_t pocket_overlay_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    overlay_limits[2].number=region.w;
    overlay_limits[3].number=region.h;
    pocket_api_register(&overlay_capability);
    return pocket_api_lazy(ctx,"overlay",build_overlay,NULL);
}
