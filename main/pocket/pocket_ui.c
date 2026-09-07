#include "pocket_ui.h"
#include "pocket_api.h"
#include "app_session.h"
#include "board.h"
#include "jsfont.h"
#include "jpfont.h"
#include "utf8.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "pocket.ui";

// ---------------------------------------------------------------- budgets
//
// Section 14's initial UI allowances. They are enforced rather than merely
// published: every number here is checked below before a node or a string is
// accepted, which is what section 2 asks a capability's limits to mean.

// Section 14 proposes 64 live nodes. This board cannot pay for 64, and the
// number below is what it can: see layout_block() for the measurement. The
// difference is not a style choice -- 25 nodes took the firmware down.
#define UI_MAX_NODES      32    // live core nodes, containers and list rows too
#define UI_SAFE_NODES     15    // below this the relayout asks for no big block
#define UI_MAX_SCREENS     4    // section 14's screen depth
#define UI_MAX_LISTS       2
#define UI_LIST_ROWS       8    // rows drawn at once, whatever the item count
#define UI_MAX_LIST_ITEMS 128   // bounds select()'s linear scan by id
#define UI_MAX_TEXT_BYTES 1024
#define UI_ACTION_SUBS     4
#define UI_TOAST_MAX_MS 5000
#define UI_TOAST_MS     1500

// Node types and property ids of the PocketJS core, from engine/core/src/spec.rs.
// The whole point of this file is that no app writes these numbers again.
#define TYPE_VIEW 0u
#define TYPE_TEXT 1u
#define P_WIDTH      1u
#define P_HEIGHT     2u
#define P_POS_TYPE  24u   // 1 = absolute
#define P_INSET_T   25u
#define P_INSET_L   28u
#define P_DISPLAY   29u   // 1 = none, out of layout and out of paint
// Hidden scissors a node's DESCENDANTS, not its own glyph run: the core emits
// the run before it pushes the child scissor (engine/core/src/draw.rs). So a
// screen clips everything inside it and a list clips its rows, but a label's own
// rectangle does not clip its text -- section 6 asks for that and this build does
// not do it. The fix is a clip box per label, which is a second node each, and
// the node tree is the one budget this board cannot afford: see layout_block().
// Overflowing text is therefore drawn, clipped to its screen.
#define P_OVERFLOW  30u
#define P_Z_INDEX   31u
#define P_BG_COLOR  64u
#define P_RADIUS    68u
#define P_TEXT_COLOR 96u
#define P_FONT_SLOT  97u
#define P_TEXT_ALIGN 98u  // 2 = right

// The pad bits pocketjs_ui_input_t.buttons carries, from
// .cache/pocketjs/contracts/generated/pocket_spec.h.
#define BTN_UP     0x0010u
#define BTN_RIGHT  0x0020u
#define BTN_DOWN   0x0040u
#define BTN_LEFT   0x0080u
#define BTN_CIRCLE 0x2000u
#define BTN_CROSS  0x4000u

#define REPEAT_DELAY_US  400000
#define REPEAT_PERIOD_US 120000

// ------------------------------------------------------- the layout cliff
//
// The Rust layout keeps its nodes in ONE doubling Vec of ~1853-byte entries
// (taffy 0.11, whose per-node measurement cache is most of that). So a relayout
// asks for one CONTIGUOUS block whose size steps as the tree grows. Measured on
// the host against this exact upstream, with a counting allocator, over trees of
// 1..40 nodes:
//
//   up to 16 taffy nodes   nothing over 1 KiB
//   17 .. 33               29,648 bytes
//   34 and up              59,296 bytes
//
// A running guest leaves about 23.5 KiB as the largest free block, so the FIRST
// step is already out of reach on a normal run. pocketjs_idf_rust_alloc returns
// NULL for a block that big and Rust aborts rather than reporting it, which is
// the "Rust core panicked" reboot. Nothing can catch that after the fact, so the
// tree is not allowed to grow into a step this heap cannot pay for -- section 6's
// "check the native budget first, keep the old display when it does not fit",
// applied to the node tree and not only to the font atlas.
//
// Legacy apps never met this: hello has 6 nodes and imucal 8. It is a limit of
// the device, not of this surface, and it is why UI_MAX_NODES is 32 and not 64.
static size_t layout_block(unsigned nodes) {
    unsigned taffy=nodes+1;            // the root counts too
    if(taffy<=16) return 0;
    if(taffy<=33) return 29648;
    return 59296;
}
// Headroom over the block itself, for the draw list, the damage plan and the
// strip work that follow in the same frame.
#define LAYOUT_MARGIN 8192

// ------------------------------------------------------------------ fonts
//
// Section 6 names small (latin), body (Japanese 12px) and compact (Japanese
// 8px). Two of the three exist on this build:
//
//   small   slot 0, the generated 6x8 latin atlas, U+0020..U+007E.
//   large   slot 1, the same face at 2x. NOT a section 6 name; it is here
//           because acceptance condition 17.1 asks the new-API Hello World to
//           show what the legacy one shows, and the legacy one titles itself in
//           slot 1. A name the document does not define is the smaller sin.
//   body    slot 2, jsfont.c's dynamic Japanese atlas, shinonome 12x12.
//
// compact would need a second dynamic slot fed from JPFONT_SMALL, and jsfont.c
// owns exactly one such slot at 12px. Asking for it fails with UNSUPPORTED
// rather than quietly drawing 12px, because section 6 forbids changing the size
// the app asked for.
//
// ascii_only is what lets a latin screen cost nothing: those slots hold a fixed
// atlas, so their text goes through ui.replaceText and never touches jsfont.
typedef struct {
    const char *name;
    uint8_t     slot;
    bool        ascii_only;
    uint8_t     line;      // px per line, for list row heights
} font_t;
static const font_t FONTS[] = {
    {"small", 0,           true,   8},
    {"large", 1,           true,  16},
    {"body",  JSFONT_SLOT, false, 12},
};
#define FONT_COUNT (int)(sizeof(FONTS)/sizeof(FONTS[0]))
#define FONT_BODY  2

// ----------------------------------------------------------------- actions

static const struct { const char *name; uint32_t bit; } ACTIONS[] = {
    {"left",   BTN_LEFT},   {"right", BTN_RIGHT},
    {"up",     BTN_UP},     {"down",  BTN_DOWN},
    {"accept", BTN_CROSS},  {"back",  BTN_CIRCLE},
};
#define ACTION_COUNT (int)(sizeof(ACTIONS)/sizeof(ACTIONS[0]))

// ------------------------------------------------------- the legacy binding
//
// See the header for why every mutation goes through globalThis.ui. The lookup
// is lazy because app_session.c installs this surface before it mounts the
// binding; the first pocket.ui call happens during the app's own evaluation,
// which is after the mount.

enum { F_CREATE, F_DESTROY, F_INSERT, F_REMOVE, F_PROP, F_SETTEXT, F_REPLACE,
       F_COUNT };
static const char *const UI_FN[F_COUNT] = {
    "createNode", "destroyNode", "insertBefore", "removeChild", "setProp",
    "setText", "replaceText",
};
static JSValue    ui_obj;
static JSValue    ui_fn[F_COUNT];
static bool       ui_ready;
// Whether the four classes and the node/list/screen tables have been made for
// this realm. They are, on the first read of either pocket.ui or pocket.input:
// both namespaces are this file's and both can reach the same state -- an
// input.text prompt opens a node -- so whichever is read first pays for them
// and the second finds them done.
static bool       realm_ready;
static JSContext *ui_ctx;
static unsigned   live_nodes;
// The legacy createNode/destroyNode as they were before pocket_ui_attach()
// replaced them. ui_bind() takes these rather than re-reading the object, so
// this surface's own calls skip the wrapper and are counted once, here.
static JSValue    legacy_create=JS_UNDEFINED, legacy_destroy=JS_UNDEFINED;
static bool       legacy_wrapped;
// The ids the wrapper handed out. ui.destroyNode has no return value to
// trust: the core drops the call silently for the root, for a dead id and
// for one that never existed (engine/core/src/lib.rs destroy_node), and the
// binding answers undefined either way. Crediting the budget for a destroy
// that destroyed nothing let four lines of legacy JS walk the counter back
// to zero with the tree still standing, and the abort this guard exists to
// prevent came back. 0 marks a free entry, which is also the id an app uses
// as its "no node yet" sentinel.
static int32_t    legacy_ids[UI_MAX_NODES];

static bool ui_bind(JSContext *ctx, const char *op) {
    if(ui_ready) return true;
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue obj=JS_GetPropertyStr(ctx,global,"ui");
    JS_FreeValue(ctx,global);
    int found=0;
    if(JS_IsObject(obj)) {
        for(;found<F_COUNT;found++) {
            // The originals, when the guest's two are wrappers: going through
            // them would count this surface's nodes twice and ask budget_ok
            // about a tree it is already inside.
            if(legacy_wrapped && found==F_CREATE)
                ui_fn[found]=JS_DupValue(ctx,legacy_create);
            else if(legacy_wrapped && found==F_DESTROY)
                ui_fn[found]=JS_DupValue(ctx,legacy_destroy);
            else
                ui_fn[found]=JS_GetPropertyStr(ctx,obj,UI_FN[found]);
            if(!JS_IsFunction(ctx,ui_fn[found])) break;
        }
    }
    if(found<F_COUNT) {
        for(int i=0;i<=found && i<F_COUNT;i++) JS_FreeValue(ctx,ui_fn[i]);
        JS_FreeValue(ctx,obj);
        pocket_api_throw(ctx,POCKET_ERR_NOT_AVAILABLE,op,
                         "the UI binding is not mounted",false,NULL);
        return false;
    }
    ui_obj=obj; ui_ctx=ctx; ui_ready=true;
    return true;
}

// The arguments below are always host-built integers, so the only way one of
// these can throw is the guest heap running out inside the binding. That is
// reported and swallowed: a half-applied node is still a node the caller owns
// and will destroy, and turning every property write into an error path would
// double the size of this file for a case the app cannot act on.
static void ui_void(JSContext *ctx, int fn, int argc, JSValue *argv) {
    JSValue result=JS_Call(ctx,ui_fn[fn],ui_obj,argc,(JSValueConst *)argv);
    if(JS_IsException(result)) {
        JS_FreeValue(ctx,JS_GetException(ctx));
        ESP_LOGW(TAG,"ui.%s failed",UI_FN[fn]);
    }
    JS_FreeValue(ctx,result);
    for(int i=0;i<argc;i++) JS_FreeValue(ctx,argv[i]);
}

static void core_prop(JSContext *ctx, int32_t id, uint32_t prop, double value) {
    JSValue a[3]={JS_NewInt32(ctx,id),JS_NewUint32(ctx,prop),
                  JS_NewFloat64(ctx,value)};
    ui_void(ctx,F_PROP,3,a);
}

static void core_insert(JSContext *ctx, int32_t parent, int32_t child) {
    if(parent<=0 || child<=0) return;
    JSValue a[3]={JS_NewInt32(ctx,parent),JS_NewInt32(ctx,child),
                  JS_NewInt32(ctx,0)};       // anchor 0 appends
    ui_void(ctx,F_INSERT,3,a);
}

static void core_detach(JSContext *ctx, int32_t parent, int32_t child) {
    if(parent<=0 || child<=0) return;
    JSValue a[2]={JS_NewInt32(ctx,parent),JS_NewInt32(ctx,child)};
    ui_void(ctx,F_REMOVE,2,a);
}

static void core_destroy(JSContext *ctx, int32_t id) {
    if(id<=0) return;
    JSValue a[1]={JS_NewInt32(ctx,id)};
    ui_void(ctx,F_DESTROY,1,a);
    if(live_nodes) live_nodes--;
}

// Whether the tree may grow by `add` nodes: the published cap, and then the
// heap, because crossing a layout step is what asks for the big contiguous
// block. Throws and returns false when it may not, so the caller applies
// nothing and the old display stands.
//
// The check is at create time and the block is taken at the next relayout, so
// this is a guard and not a guarantee: an app that grows the font atlas in
// between can still lose the race. It turns a certain abort into a very likely
// clean refusal, which is the most an app-side check can do against an
// allocator that answers NULL into a panic.
static bool budget_ok(JSContext *ctx, unsigned add, const char *op) {
    if(live_nodes+add>UI_MAX_NODES) {
        pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,
                         "the display node budget is spent",false,
                         POCKET_OUTCOME_NOT_APPLIED);
        return false;
    }
    size_t need=layout_block(live_nodes+add);
    if(need>layout_block(live_nodes) &&
       heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)
       < need+LAYOUT_MARGIN) {
        pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,op,
                         "the layout needs one contiguous block this heap has "
                         "not got; use fewer nodes",false,
                         POCKET_OUTCOME_NOT_APPLIED);
        return false;
    }
    return true;
}

// -1 when the node budget is spent or the core refused; the caller reports
// LIMIT_EXCEEDED and applies nothing.
static int32_t core_create(JSContext *ctx, uint32_t type) {
    if(live_nodes>=UI_MAX_NODES) return -1;
    JSValue argument=JS_NewUint32(ctx,type);
    JSValue result=JS_Call(ctx,ui_fn[F_CREATE],ui_obj,1,
                           (JSValueConst *)&argument);
    JS_FreeValue(ctx,argument);
    int32_t id=-1;
    if(JS_IsException(result)) JS_FreeValue(ctx,JS_GetException(ctx));
    else JS_ToInt32(ctx,&id,result);
    JS_FreeValue(ctx,result);
    if(id<=0) return -1;
    live_nodes++;
    return id;
}

// The rectangle every node of this surface is placed by: absolute, clipped, and
// in display pixels, because every screen container is the whole display.
static void core_box(JSContext *ctx, int32_t id, double x, double y,
                     double w, double h) {
    if(id<=0) return;
    core_prop(ctx,id,P_POS_TYPE,1);
    core_prop(ctx,id,P_INSET_L,x);
    core_prop(ctx,id,P_INSET_T,y);
    core_prop(ctx,id,P_WIDTH,w);
    core_prop(ctx,id,P_HEIGHT,h);
    core_prop(ctx,id,P_OVERFLOW,1);
}

// ------------------------------------------------------------------- tables

typedef struct {
    JSValue  items;        // the app's array, retained until remove()
    int32_t  container, highlight;
    int32_t  label[UI_LIST_ROWS], detail[UI_LIST_ROWS];
    int32_t  count, selected, scroll;
    int16_t  row_h;
    uint8_t  rows, font;
    bool     used;
} list_t;

typedef struct {
    uint32_t handle;       // 0 marks a free entry
    int32_t  id;
    uint8_t  screen;
    uint8_t  font;
    uint8_t  list;         // index into lists[], 0xff when this is not a list
} node_t;

typedef struct {
    uint32_t handle;       // 0 marks a free entry
    int32_t  container;
    bool     pushed;
} screen_t;

// A handle is (serial << index bits) | index, exactly the trick pocket_api.c
// uses for a request number: the serial never repeats within a run, so a
// wrapper naming a slot that has since been reused finds a mismatch and reports
// CLOSED instead of quietly driving somebody else's node.
#define NODE_BITS   6
#define SCREEN_BITS 2
static node_t   nodes[UI_MAX_NODES];
static list_t   lists[UI_MAX_LISTS];
static screen_t screens[UI_MAX_SCREENS];
static uint8_t  stack[UI_MAX_SCREENS];      // screen indices, bottom first
static uint8_t  depth;
static uint32_t node_serial=1, screen_serial=1;

static JSClassID screen_class, node_class, text_class, list_class;

static node_t *node_of(uint32_t handle) {
    node_t *n=&nodes[handle&((1u<<NODE_BITS)-1)];
    return (handle && n->handle==handle)?n:NULL;
}
static screen_t *screen_of(uint32_t handle) {
    screen_t *s=&screens[handle&((1u<<SCREEN_BITS)-1)];
    return (handle && s->handle==handle)?s:NULL;
}
static uint8_t screen_index(const screen_t *s) { return (uint8_t)(s-screens); }

// A JS wrapper carries its handle in the class opaque, which JS can neither
// read nor forge. A handle is never 0, so "no opaque" and "not one of ours" are
// the same answer.
static uint32_t handle_of(JSValueConst value, JSClassID class_id) {
    return (uint32_t)(uintptr_t)JS_GetOpaque(value,class_id);
}

// ----------------------------------------------------------- argument shapes

static JSValue bad(JSContext *ctx, const char *op, const char *what) {
    return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,op,what,false,NULL);
}

// One finite number out of a spec object. `fallback` is used when the property
// is absent; pass NAN to make it required.
static bool spec_num(JSContext *ctx, JSValueConst spec, const char *name,
                     double fallback, double *out, const char *op) {
    char note[64];
    JSValue value=JS_GetPropertyStr(ctx,spec,name);
    if(JS_IsException(value)) return false;
    if(JS_IsUndefined(value)) {
        JS_FreeValue(ctx,value);
        if(!isnan(fallback)) { *out=fallback; return true; }
        snprintf(note,sizeof(note),"%s is required",name);
        bad(ctx,op,note);
        return false;
    }
    int failed=JS_ToFloat64(ctx,out,value);
    JS_FreeValue(ctx,value);
    if(failed) return false;
    if(!isfinite(*out)) {
        snprintf(note,sizeof(note),"%s must be a finite number",name);
        bad(ctx,op,note);
        return false;
    }
    return true;
}

// Geometry is bounded rather than accepted blindly: a node parked at 1e9 px
// costs the same layout pass as one on screen and shows nothing, so the range
// is where an app can still see what it did.
static bool spec_coord(JSContext *ctx, JSValueConst spec, const char *name,
                       double fallback, double *out, const char *op,
                       double lo, double hi) {
    char note[64];
    if(!spec_num(ctx,spec,name,fallback,out,op)) return false;
    if(*out<lo || *out>hi) {
        snprintf(note,sizeof(note),"%s is outside %d..%d",name,(int)lo,(int)hi);
        bad(ctx,op,note);
        return false;
    }
    return true;
}

static bool spec_color(JSContext *ctx, JSValueConst spec, const char *name,
                       double fallback, uint32_t *out, const char *op) {
    double value;
    if(!spec_num(ctx,spec,name,fallback,&value,op)) return false;
    if(value<0 || value>4294967295.0) {
        bad(ctx,op,"colour is 0xRRGGBBAA");
        return false;
    }
    *out=(uint32_t)value;
    return true;
}

static bool spec_font(JSContext *ctx, JSValueConst spec, uint8_t *out,
                      const char *op) {
    JSValue value=JS_GetPropertyStr(ctx,spec,"font");
    if(JS_IsException(value)) return false;
    if(JS_IsUndefined(value)) { JS_FreeValue(ctx,value); *out=FONT_BODY; return true; }
    const char *name=JS_ToCString(ctx,value);
    JS_FreeValue(ctx,value);
    if(!name) return false;
    for(int i=0;i<FONT_COUNT;i++) {
        if(strcmp(name,FONTS[i].name)) continue;
        JS_FreeCString(ctx,name);
        *out=(uint8_t)i;
        return true;
    }
    // compact is a section 6 name this build has no atlas slot for; any other
    // name is simply wrong. The two get different codes so that a program
    // feature-testing for compact can tell them apart.
    bool named=!strcmp(name,"compact");
    JS_FreeCString(ctx,name);
    pocket_api_throw(ctx,named?POCKET_ERR_UNSUPPORTED:POCKET_ERR_INVALID_ARGUMENT,
                     op,named?"font compact needs an 8px atlas slot this build "
                              "does not have"
                             :"font must be small, body or large",false,NULL);
    return false;
}

// ---------------------------------------------------------------- node text
//
// Section 6 wants the native budget checked before the change and the old
// display kept when it does not fit, so nothing reaches the core until the
// length, the encoding and the font atlas headroom have all passed.

static bool node_text(JSContext *ctx, int32_t id, uint8_t font,
                      JSValueConst text, const char *op) {
    size_t      bytes=0;
    const char *s=JS_ToCStringLen(ctx,&bytes,text);
    if(!s) return false;
    if(bytes>UI_MAX_TEXT_BYTES) {
        JS_FreeCString(ctx,s);
        pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,
                         "one string is at most 1024 UTF-8 bytes",false,
                         POCKET_OUTCOME_NOT_APPLIED);
        return false;
    }
    unsigned chars=0;
    for(size_t i=0;i<bytes;) {
        size_t   advance;
        uint32_t cp=utf8_decode(s,bytes,i,&advance);
        i+=advance;
        chars++;
        // QuickJS hands a lone surrogate over in its three-byte form; section 4
        // says a character API refuses that rather than drawing half a pair.
        if(cp>=0xd800 && cp<=0xdfff) {
            JS_FreeCString(ctx,s);
            bad(ctx,op,"text is not valid UTF-8");
            return false;
        }
        // The latin atlases hold U+0020..U+007E and nothing else, so anything
        // outside that range would draw a wrong cell rather than nothing.
        // Newline is the one control character section 6 gives a meaning to.
        if(FONTS[font].ascii_only && cp!='\n' && (cp<0x20 || cp>0x7e)) {
            JS_FreeCString(ctx,s);
            bad(ctx,op,"fonts small and large hold ASCII only; use body");
            return false;
        }
    }

    // The one expensive thing a text API on this board can do. jsfont.c rebuilds
    // the whole Japanese slot whenever a new character appears, and the core's
    // Rust side aborts the firmware instead of reporting an allocation failure,
    // so the rebuild is refused here while refusing is still possible. `chars`
    // is the worst case for how many glyphs this string can add; jsfont's own
    // note puts the peak at about three times the finished atlas, the new blob
    // and the core's copy being alive while the old copy still is.
    if(!FONTS[font].ascii_only && chars && jsfont_count()<JSFONT_MAX) {
        unsigned cell=jpfont_cell_w(JPFONT_TEXT)*jpfont_cell_h(JPFONT_TEXT);
        if(cell) {
            unsigned glyphs=jsfont_count()+chars+1;
            if(glyphs>JSFONT_MAX+1) glyphs=JSFONT_MAX+1;
            size_t need=3u*(16u+(size_t)glyphs*(8u+cell));
            if(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)
               < need) {
                JS_FreeCString(ctx,s);
                pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,op,
                                 "not enough contiguous memory to grow the font "
                                 "atlas",true,POCKET_OUTCOME_NOT_APPLIED);
                return false;
            }
        }
    }
    JS_FreeCString(ctx,s);

    // setText is jsfont.c's wrapper and registers the glyphs; replaceText is the
    // same core call with no wrapper on it. A latin string therefore costs no
    // atlas work at all, which is the largest single difference between this
    // surface and the legacy one it replaces.
    JSValue a[2]={JS_NewInt32(ctx,id),JS_DupValue(ctx,text)};
    ui_void(ctx,FONTS[font].ascii_only?F_REPLACE:F_SETTEXT,2,a);
    return true;
}

// ------------------------------------------------------------------- lists

static bool list_render(JSContext *ctx, list_t *l, const char *op) {
    for(int r=0;r<l->rows;r++) {
        int32_t index=l->scroll+r;
        JSValue item=index<l->count
            ?JS_GetPropertyUint32(ctx,l->items,(uint32_t)index):JS_UNDEFINED;
        bool    have=JS_IsObject(item);
        JSValue label=have?JS_GetPropertyStr(ctx,item,"label"):JS_UNDEFINED;
        JSValue detail=have?JS_GetPropertyStr(ctx,item,"detail"):JS_UNDEFINED;
        bool    has_detail=have && JS_IsString(detail);
        bool    ok=true;
        core_prop(ctx,l->label[r],P_DISPLAY,have?0:1);
        core_prop(ctx,l->detail[r],P_DISPLAY,has_detail?0:1);
        if(have) ok=node_text(ctx,l->label[r],l->font,label,op);
        if(ok && has_detail) ok=node_text(ctx,l->detail[r],l->font,detail,op);
        JS_FreeValue(ctx,label);
        JS_FreeValue(ctx,detail);
        JS_FreeValue(ctx,item);
        if(!ok) return false;
    }
    bool visible=l->selected>=l->scroll && l->selected<l->scroll+l->rows;
    core_prop(ctx,l->highlight,P_DISPLAY,visible?0:1);
    if(visible) core_prop(ctx,l->highlight,P_INSET_T,
                          (double)(l->selected-l->scroll)*l->row_h);
    return true;
}

// Only the selection moves the window, and only far enough to bring it back.
static void list_follow(list_t *l) {
    if(l->selected<0) return;
    if(l->selected<l->scroll) l->scroll=l->selected;
    else if(l->selected>=l->scroll+l->rows) l->scroll=l->selected-l->rows+1;
    if(l->scroll>l->count-l->rows) l->scroll=l->count-l->rows;
    if(l->scroll<0) l->scroll=0;
}

// Retains `items` on success. Every item is checked before any of it is shown,
// so a malformed array leaves the list exactly as it was.
static bool list_items(JSContext *ctx, list_t *l, JSValueConst items,
                       const char *op) {
    int64_t count=0;
    if(!JS_IsArray(items)) { bad(ctx,op,"items must be an array"); return false; }
    if(JS_GetLength(ctx,items,&count)) return false;
    if(count>UI_MAX_LIST_ITEMS) {
        pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,"too many items",false,
                         POCKET_OUTCOME_NOT_APPLIED);
        return false;
    }
    for(int64_t i=0;i<count;i++) {
        JSValue item=JS_GetPropertyUint32(ctx,items,(uint32_t)i);
        JSValue id=JS_IsObject(item)?JS_GetPropertyStr(ctx,item,"id"):JS_UNDEFINED;
        JSValue label=JS_IsObject(item)?JS_GetPropertyStr(ctx,item,"label")
                                       :JS_UNDEFINED;
        bool ok=JS_IsString(id) && JS_IsString(label);
        JS_FreeValue(ctx,id);
        JS_FreeValue(ctx,label);
        JS_FreeValue(ctx,item);
        if(!ok) {
            bad(ctx,op,"each item needs a string id and a string label");
            return false;
        }
    }
    JS_FreeValue(ctx,l->items);
    l->items=JS_DupValue(ctx,items);
    l->count=(int32_t)count;
    if(l->selected>=l->count) l->selected=l->count?l->count-1:-1;
    list_follow(l);
    return list_render(ctx,l,op);
}

static void list_free(JSContext *ctx, list_t *l) {
    if(!l->used) return;
    JS_FreeValue(ctx,l->items);
    // Destroying the container takes its children with it in the core, so only
    // the budget has to be told about them here.
    core_destroy(ctx,l->container);
    unsigned owned=(unsigned)(1+l->rows*2);      // highlight plus the row nodes
    live_nodes=live_nodes>owned?live_nodes-owned:0;
    memset(l,0,sizeof(*l));
}

// ------------------------------------------------------------- node objects

static node_t *claim_node(JSContext *ctx, uint8_t screen, const char *op) {
    for(int i=0;i<UI_MAX_NODES;i++) {
        if(nodes[i].handle) continue;
        if(++node_serial==0) node_serial=1;
        nodes[i]=(node_t){.handle=(node_serial<<NODE_BITS)|(uint32_t)i,
                          .screen=screen,.list=0xff};
        return &nodes[i];
    }
    pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,"too many nodes",false,
                     POCKET_OUTCOME_NOT_APPLIED);
    return NULL;
}

static void drop_node(JSContext *ctx, node_t *n) {
    if(n->list!=0xff) list_free(ctx,&lists[n->list]);
    else core_destroy(ctx,n->id);
    memset(n,0,sizeof(*n));
}

static JSValue wrap_node(JSContext *ctx, node_t *n, JSClassID class_id) {
    JSValue object=JS_NewObjectClass(ctx,class_id);
    if(JS_IsException(object)) { drop_node(ctx,n); return object; }
    JS_SetOpaque(object,(void *)(uintptr_t)n->handle);
    return object;
}

// Every node method starts here: the wrapper says which slot, and the slot says
// whether it is still that node.
static node_t *this_node(JSContext *ctx, JSValueConst self, JSClassID class_id,
                         const char *op) {
    node_t *n=node_of(handle_of(self,class_id));
    if(!n) pocket_api_throw(ctx,POCKET_ERR_CLOSED,op,"this node was removed",
                            false,NULL);
    return n;
}

static JSValue js_node_remove(JSContext *ctx, JSValueConst self,
                              int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    // Any of the three classes may be the receiver, so the lookup is by handle
    // and the class only decides which prototype found it.
    node_t *n=node_of(handle_of(self,node_class));
    if(!n) n=node_of(handle_of(self,text_class));
    if(!n) n=node_of(handle_of(self,list_class));
    // remove() on an already removed node is a no-op rather than an error:
    // section 4 asks close() to be idempotent, and this is the same shape.
    if(n) { JS_SetOpaque(self,NULL); drop_node(ctx,n); }
    return JS_UNDEFINED;
}

static JSValue js_text_set(JSContext *ctx, JSValueConst self,
                           int argc, JSValueConst *argv) {
    node_t *n=this_node(ctx,self,text_class,"ui.text.setText");
    if(!n) return JS_EXCEPTION;
    if(argc<1 || !JS_IsString(argv[0]))
        return bad(ctx,"ui.text.setText","text must be a string");
    if(!node_text(ctx,n->id,n->font,argv[0],"ui.text.setText")) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue js_text_move(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    node_t *n=this_node(ctx,self,text_class,"ui.text.setPosition");
    if(!n) return JS_EXCEPTION;
    double x,y;
    if(argc<2 || JS_ToFloat64(ctx,&x,argv[0]) || JS_ToFloat64(ctx,&y,argv[1]))
        return bad(ctx,"ui.text.setPosition","setPosition(x, y) takes numbers");
    if(!isfinite(x) || !isfinite(y) ||
       x<-LCD_W || x>2*LCD_W || y<-LCD_H || y>2*LCD_H)
        return bad(ctx,"ui.text.setPosition","position is off the display");
    core_prop(ctx,n->id,P_INSET_L,x);
    core_prop(ctx,n->id,P_INSET_T,y);
    return JS_UNDEFINED;
}

static JSValue js_list_set_items(JSContext *ctx, JSValueConst self,
                                 int argc, JSValueConst *argv) {
    node_t *n=this_node(ctx,self,list_class,"ui.list.setItems");
    if(!n) return JS_EXCEPTION;
    if(argc<1) return bad(ctx,"ui.list.setItems","items must be an array");
    if(!list_items(ctx,&lists[n->list],argv[0],"ui.list.setItems"))
        return JS_EXCEPTION;
    return JS_UNDEFINED;
}

static JSValue js_list_select(JSContext *ctx, JSValueConst self,
                              int argc, JSValueConst *argv) {
    node_t *n=this_node(ctx,self,list_class,"ui.list.select");
    if(!n) return JS_EXCEPTION;
    if(argc<1 || !JS_IsString(argv[0]))
        return bad(ctx,"ui.list.select","select(id) takes the item's id");
    list_t     *l=&lists[n->list];
    const char *wanted=JS_ToCString(ctx,argv[0]);
    if(!wanted) return JS_EXCEPTION;
    int32_t found=-1;
    for(int32_t i=0;i<l->count && found<0;i++) {
        JSValue item=JS_GetPropertyUint32(ctx,l->items,(uint32_t)i);
        JSValue id=JS_GetPropertyStr(ctx,item,"id");
        const char *text=JS_ToCString(ctx,id);
        if(text && !strcmp(text,wanted)) found=i;
        if(text) JS_FreeCString(ctx,text);
        JS_FreeValue(ctx,id);
        JS_FreeValue(ctx,item);
    }
    JS_FreeCString(ctx,wanted);
    if(found<0)
        return pocket_api_throw(ctx,POCKET_ERR_NOT_FOUND,"ui.list.select",
                                "no item with that id",false,NULL);
    l->selected=found;
    list_follow(l);
    if(!list_render(ctx,l,"ui.list.select")) return JS_EXCEPTION;
    return JS_UNDEFINED;
}

// ----------------------------------------------------------- screen objects

static screen_t *this_screen(JSContext *ctx, JSValueConst self, const char *op) {
    screen_t *s=screen_of(handle_of(self,screen_class));
    if(!s) pocket_api_throw(ctx,POCKET_ERR_CLOSED,op,"this screen was closed",
                            false,NULL);
    return s;
}

static JSValue js_screen_rect(JSContext *ctx, JSValueConst self,
                              int argc, JSValueConst *argv) {
    static const char *const OP="ui.screen.rect";
    screen_t *s=this_screen(ctx,self,OP);
    if(!s) return JS_EXCEPTION;
    if(argc<1 || !JS_IsObject(argv[0]))
        return bad(ctx,OP,"rect(spec) takes an object");
    double   x,y,w,h,radius;
    uint32_t colour;
    if(!spec_coord(ctx,argv[0],"x",NAN,&x,OP,-LCD_W,2*LCD_W) ||
       !spec_coord(ctx,argv[0],"y",NAN,&y,OP,-LCD_H,2*LCD_H) ||
       !spec_coord(ctx,argv[0],"width",NAN,&w,OP,0,2*LCD_W) ||
       !spec_coord(ctx,argv[0],"height",NAN,&h,OP,0,2*LCD_H) ||
       !spec_coord(ctx,argv[0],"radius",0,&radius,OP,0,64) ||
       !spec_color(ctx,argv[0],"color",NAN,&colour,OP)) return JS_EXCEPTION;
    if(!budget_ok(ctx,1,OP)) return JS_EXCEPTION;
    node_t *n=claim_node(ctx,screen_index(s),OP);
    if(!n) return JS_EXCEPTION;
    n->id=core_create(ctx,TYPE_VIEW);
    if(n->id<0) {
        memset(n,0,sizeof(*n));
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                "the display node budget is spent",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    }
    core_box(ctx,n->id,x,y,w,h);
    core_prop(ctx,n->id,P_BG_COLOR,colour);
    if(radius>0) core_prop(ctx,n->id,P_RADIUS,radius);
    core_insert(ctx,s->container,n->id);
    return wrap_node(ctx,n,node_class);
}

static JSValue js_screen_text(JSContext *ctx, JSValueConst self,
                              int argc, JSValueConst *argv) {
    static const char *const OP="ui.screen.text";
    screen_t *s=this_screen(ctx,self,OP);
    if(!s) return JS_EXCEPTION;
    if(argc<1 || !JS_IsObject(argv[0]))
        return bad(ctx,OP,"text(spec) takes an object");
    double   x,y,w,h;
    uint32_t colour;
    uint8_t  font;
    if(!spec_coord(ctx,argv[0],"x",NAN,&x,OP,-LCD_W,2*LCD_W) ||
       !spec_coord(ctx,argv[0],"y",NAN,&y,OP,-LCD_H,2*LCD_H) ||
       !spec_coord(ctx,argv[0],"width",NAN,&w,OP,0,2*LCD_W) ||
       !spec_coord(ctx,argv[0],"height",NAN,&h,OP,0,2*LCD_H) ||
       !spec_color(ctx,argv[0],"color",NAN,&colour,OP) ||
       !spec_font(ctx,argv[0],&font,OP)) return JS_EXCEPTION;
    JSValue text=JS_GetPropertyStr(ctx,argv[0],"text");
    if(!JS_IsString(text)) {
        JS_FreeValue(ctx,text);
        return bad(ctx,OP,"text is required and must be a string");
    }
    if(!budget_ok(ctx,1,OP)) { JS_FreeValue(ctx,text); return JS_EXCEPTION; }
    node_t *n=claim_node(ctx,screen_index(s),OP);
    if(!n) { JS_FreeValue(ctx,text); return JS_EXCEPTION; }
    n->font=font;
    n->id=core_create(ctx,TYPE_TEXT);
    if(n->id<0) {
        memset(n,0,sizeof(*n));
        JS_FreeValue(ctx,text);
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                "the display node budget is spent",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    }
    core_box(ctx,n->id,x,y,w,h);
    core_prop(ctx,n->id,P_TEXT_COLOR,colour);
    core_prop(ctx,n->id,P_FONT_SLOT,FONTS[font].slot);
    bool ok=node_text(ctx,n->id,font,text,OP);
    JS_FreeValue(ctx,text);
    if(!ok) { drop_node(ctx,n); return JS_EXCEPTION; }
    core_insert(ctx,s->container,n->id);
    return wrap_node(ctx,n,text_class);
}

static JSValue js_screen_list(JSContext *ctx, JSValueConst self,
                              int argc, JSValueConst *argv) {
    static const char *const OP="ui.screen.list";
    screen_t *s=this_screen(ctx,self,OP);
    if(!s) return JS_EXCEPTION;
    if(argc<1 || !JS_IsObject(argv[0]))
        return bad(ctx,OP,"list(spec) takes an object");
    double  x,y,w,h;
    uint8_t font;
    if(!spec_coord(ctx,argv[0],"x",NAN,&x,OP,-LCD_W,2*LCD_W) ||
       !spec_coord(ctx,argv[0],"y",NAN,&y,OP,-LCD_H,2*LCD_H) ||
       !spec_coord(ctx,argv[0],"width",NAN,&w,OP,0,2*LCD_W) ||
       !spec_coord(ctx,argv[0],"height",NAN,&h,OP,0,2*LCD_H) ||
       !spec_font(ctx,argv[0],&font,OP)) return JS_EXCEPTION;
    int slot=-1;
    for(int i=0;i<UI_MAX_LISTS;i++) if(!lists[i].used) { slot=i; break; }
    if(slot<0)
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,"too many lists",
                                false,POCKET_OUTCOME_NOT_APPLIED);
    int row_h=FONTS[font].line+2;
    int rows=(int)h/row_h;
    if(rows>UI_LIST_ROWS) rows=UI_LIST_ROWS;
    if(rows<1) rows=1;
    // Rows are allocated once and re-texted on every change: what a list costs
    // the node budget depends on how tall it is, never on how many items it
    // holds. That is the whole reason scrolling lives in here and not in the app.
    // Container, highlight, and a label and a detail per row.
    if(!budget_ok(ctx,(unsigned)(2+rows*2),OP)) return JS_EXCEPTION;
    node_t *n=claim_node(ctx,screen_index(s),OP);
    if(!n) return JS_EXCEPTION;
    list_t *l=&lists[slot];
    memset(l,0,sizeof(*l));
    l->used=true; l->items=JS_UNDEFINED; l->selected=-1; l->font=font;
    l->rows=(uint8_t)rows; l->row_h=(int16_t)row_h;
    n->list=(uint8_t)slot;
    l->container=core_create(ctx,TYPE_VIEW);
    l->highlight=core_create(ctx,TYPE_VIEW);
    n->id=l->container;
    core_box(ctx,l->container,x,y,w,h);
    // Inside the container, so the origin is the container's, not the screen's.
    core_box(ctx,l->highlight,0,0,w,row_h);
    core_prop(ctx,l->highlight,P_BG_COLOR,0x2a5f8fffu);
    core_prop(ctx,l->highlight,P_RADIUS,2);
    core_prop(ctx,l->highlight,P_DISPLAY,1);
    core_insert(ctx,l->container,l->highlight);
    for(int r=0;r<rows;r++) {
        l->label[r]=core_create(ctx,TYPE_TEXT);
        l->detail[r]=core_create(ctx,TYPE_TEXT);
        // Insets count from the container, which is the one node here with
        // overflow hidden: rows are clipped to the list, not to each other.
        core_box(ctx,l->label[r],4,(double)r*row_h+1,w-8,row_h-2);
        core_box(ctx,l->detail[r],w/2,(double)r*row_h+1,w/2-4,row_h-2);
        core_prop(ctx,l->label[r],P_FONT_SLOT,FONTS[font].slot);
        core_prop(ctx,l->detail[r],P_FONT_SLOT,FONTS[font].slot);
        core_prop(ctx,l->label[r],P_TEXT_COLOR,0xf0f8ffffu);
        core_prop(ctx,l->detail[r],P_TEXT_COLOR,0xa9bacaffu);
        core_prop(ctx,l->detail[r],P_TEXT_ALIGN,2);
        core_prop(ctx,l->label[r],P_DISPLAY,1);
        core_prop(ctx,l->detail[r],P_DISPLAY,1);
        core_insert(ctx,l->container,l->label[r]);
        core_insert(ctx,l->container,l->detail[r]);
    }
    core_insert(ctx,s->container,l->container);
    JSValue items=JS_GetPropertyStr(ctx,argv[0],"items");
    bool    ok=list_items(ctx,l,items,OP);
    JS_FreeValue(ctx,items);
    if(!ok) { drop_node(ctx,n); return JS_EXCEPTION; }
    JSValue wrapper=wrap_node(ctx,n,list_class);
    if(JS_IsException(wrapper)) return wrapper;
    JSValue selected=JS_GetPropertyStr(ctx,argv[0],"selected");
    if(JS_IsString(selected)) {
        JSValue result=js_list_select(ctx,wrapper,1,(JSValueConst *)&selected);
        JS_FreeValue(ctx,selected);
        if(JS_IsException(result)) { JS_FreeValue(ctx,wrapper); return result; }
        JS_FreeValue(ctx,result);
        return wrapper;
    }
    JS_FreeValue(ctx,selected);
    return wrapper;
}

static void screen_drop(JSContext *ctx, screen_t *s) {
    uint8_t index=screen_index(s);
    bool    was_top=depth && stack[depth-1]==index;
    for(int i=0;i<UI_MAX_NODES;i++)
        if(nodes[i].handle && nodes[i].screen==index) drop_node(ctx,&nodes[i]);
    for(uint8_t d=0;d<depth;d++) {
        if(stack[d]!=index) continue;
        memmove(stack+d,stack+d+1,(size_t)(depth-d-1)*sizeof(stack[0]));
        depth--;
        break;
    }
    core_destroy(ctx,s->container);
    memset(s,0,sizeof(*s));
    // Only the top screen is ever attached, so only losing the top one leaves
    // the root without a screen to show.
    if(was_top && depth) core_insert(ctx,1,screens[stack[depth-1]].container);
}

static JSValue js_screen_close(JSContext *ctx, JSValueConst self,
                               int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    screen_t *s=screen_of(handle_of(self,screen_class));
    if(s) { JS_SetOpaque(self,NULL); screen_drop(ctx,s); }
    return JS_UNDEFINED;
}

// -------------------------------------------------------------- pocket.ui

static JSValue js_ui_screen(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    static const char *const OP="ui.screen";
    (void)self;
    if(!ui_bind(ctx,OP)) return JS_EXCEPTION;
    uint32_t background=0x000000ffu;
    if(argc>0 && JS_IsObject(argv[0]) &&
       !spec_color(ctx,argv[0],"background",255,&background,OP))
        return JS_EXCEPTION;
    int index=-1;
    for(int i=0;i<UI_MAX_SCREENS;i++) if(!screens[i].handle) { index=i; break; }
    if(index<0)
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                "too many screens",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    if(!budget_ok(ctx,1,OP)) return JS_EXCEPTION;
    int32_t container=core_create(ctx,TYPE_VIEW);
    if(container<0)
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                "the display node budget is spent",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    // The container is the whole display and carries the background, so a
    // child's x and y are screen pixels and push() is one insert.
    core_box(ctx,container,0,0,LCD_W,LCD_H);
    core_prop(ctx,container,P_BG_COLOR,background);
    if(++screen_serial==0) screen_serial=1;
    screens[index]=(screen_t){
        .handle=(screen_serial<<SCREEN_BITS)|(uint32_t)index,
        .container=container};
    JSValue object=JS_NewObjectClass(ctx,screen_class);
    if(JS_IsException(object)) {
        core_destroy(ctx,container);
        memset(&screens[index],0,sizeof(screens[index]));
        return object;
    }
    JS_SetOpaque(object,(void *)(uintptr_t)screens[index].handle);
    return object;
}

static JSValue js_ui_push(JSContext *ctx, JSValueConst self,
                          int argc, JSValueConst *argv) {
    static const char *const OP="ui.push";
    (void)self;
    if(argc<1) return bad(ctx,OP,"push(screen) takes a screen");
    screen_t *s=screen_of(handle_of(argv[0],screen_class));
    if(!s) return pocket_api_throw(ctx,POCKET_ERR_CLOSED,OP,
                                   "not a live screen of this app",false,NULL);
    if(s->pushed) return bad(ctx,OP,"that screen is already pushed");
    if(depth>=UI_MAX_SCREENS)
        return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                "screen depth 4 is the limit",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    // Only the top screen is attached. The ones underneath keep their nodes and
    // cost the renderer nothing until a pop brings them back.
    if(depth) core_detach(ctx,1,screens[stack[depth-1]].container);
    stack[depth++]=screen_index(s);
    s->pushed=true;
    core_insert(ctx,1,s->container);
    return JS_UNDEFINED;
}

static JSValue js_ui_pop(JSContext *ctx, JSValueConst self,
                         int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    if(depth) screen_drop(ctx,&screens[stack[depth-1]]);
    // "Popping the last screen is app.exit". There is no pocket.app on this
    // build, so this is the host's own stop request: the interrupt handler ends
    // the turn and main.c returns to whichever screen started the run.
    if(!depth) app_request_stop();
    return JS_UNDEFINED;
}

// A toast is host furniture rather than the app's: it hangs off the root above
// every screen, so a push or a pop underneath does not disturb it, and the pump
// takes it away again.
static int32_t toast_box, toast_label;
static int64_t toast_until;

static void toast_hide(JSContext *ctx) {
    if(!toast_box) return;
    core_destroy(ctx,toast_box);   // the label is its child and goes with it
    if(live_nodes) live_nodes--;
    toast_box=0; toast_label=0; toast_until=0;
}

static JSValue js_ui_toast(JSContext *ctx, JSValueConst self,
                           int argc, JSValueConst *argv) {
    static const char *const OP="ui.toast";
    (void)self;
    if(!ui_bind(ctx,OP)) return JS_EXCEPTION;
    if(argc<1 || !JS_IsString(argv[0]))
        return bad(ctx,OP,"toast(text) takes a string");
    double ms=UI_TOAST_MS;
    if(argc>1 && JS_IsObject(argv[1]) &&
       !spec_coord(ctx,argv[1],"durationMs",UI_TOAST_MS,&ms,OP,1,UI_TOAST_MAX_MS))
        return JS_EXCEPTION;
    if(!toast_box) {
        if(!budget_ok(ctx,2,OP)) return JS_EXCEPTION;
        toast_box=core_create(ctx,TYPE_VIEW);
        toast_label=core_create(ctx,TYPE_TEXT);
        if(toast_box<0 || toast_label<0) {
            core_destroy(ctx,toast_box);
            core_destroy(ctx,toast_label);
            toast_box=0; toast_label=0;
            return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                    "the display node budget is spent",false,
                                    POCKET_OUTCOME_NOT_APPLIED);
        }
        core_box(ctx,toast_box,8,LCD_H-26,LCD_W-16,18);
        core_prop(ctx,toast_box,P_BG_COLOR,0x0b1c2ee6u);
        core_prop(ctx,toast_box,P_RADIUS,4);
        core_prop(ctx,toast_box,P_Z_INDEX,100);
        core_box(ctx,toast_label,6,3,LCD_W-28,12);
        core_prop(ctx,toast_label,P_TEXT_COLOR,0xf0f8ffffu);
        core_prop(ctx,toast_label,P_FONT_SLOT,FONTS[FONT_BODY].slot);
        core_insert(ctx,toast_box,toast_label);
        core_insert(ctx,1,toast_box);
    }
    if(!node_text(ctx,toast_label,FONT_BODY,argv[0],OP)) {
        toast_hide(ctx);
        return JS_EXCEPTION;
    }
    toast_until=esp_timer_get_time()+(int64_t)(ms*1000);
    return JS_UNDEFINED;
}

// ------------------------------------------------------------ pocket.input

static pocket_sub_slot_t  action_slots[UI_ACTION_SUBS];
static pocket_sub_table_t action_table = {
    .slots=action_slots, .count=UI_ACTION_SUBS,
    .tag="pocket.ui", .what="onAction",
    // A listener throwing on every press would fill the log and keep costing a
    // call; the app keeps its other subscriptions. Same reasoning as imu.watch.
    .close_on_throw=true,
};
static uint32_t held_mask;
static int64_t  repeat_at[ACTION_COUNT];

typedef struct { int action; const char *phase; int64_t now; } action_event_t;

static bool action_payload(JSContext *ctx, int slot, void *user, JSValue *payload) {
    const action_event_t *e=user;
    (void)slot;
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object)) return false;
    JS_SetPropertyStr(ctx,object,"action",
                      JS_NewString(ctx,ACTIONS[e->action].name));
    JS_SetPropertyStr(ctx,object,"phase",JS_NewString(ctx,e->phase));
    JS_SetPropertyStr(ctx,object,"timeMs",JS_NewFloat64(ctx,e->now/1000.0));
    *payload=object;
    return true;
}

static JSValue js_on_action(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    (void)self;
    if(argc<1 || !JS_IsFunction(ctx,argv[0]))
        return bad(ctx,"input.onAction","listener must be a function");
    return pocket_api_sub_open(ctx,&action_table,argv[0],"input.onAction",
                               "too many input subscriptions",NULL);
}

static JSValue js_held(JSContext *ctx, JSValueConst self,
                       int argc, JSValueConst *argv) {
    (void)self;
    if(argc<1 || !JS_IsString(argv[0]))
        return bad(ctx,"input.held","held(action) takes an action name");
    const char *name=JS_ToCString(ctx,argv[0]);
    if(!name) return JS_EXCEPTION;
    for(int i=0;i<ACTION_COUNT;i++) {
        if(strcmp(name,ACTIONS[i].name)) continue;
        JS_FreeCString(ctx,name);
        return JS_NewBool(ctx,(held_mask&ACTIONS[i].bit)!=0);
    }
    JS_FreeCString(ctx,name);
    return bad(ctx,"input.held","no such action");
}

// Section 6 gives onKey a key/code/modifiers shape, and this host has no channel
// for it: main.c's keymap consumes the keyboard for the shell and hands a running
// app a pad mask alone. Section 2 says an unimplemented feature keeps its name
// and fails with UNSUPPORTED, which is a better answer than a subscription that
// silently never fires.
static JSValue js_on_key(JSContext *ctx, JSValueConst self,
                         int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    return pocket_api_throw(ctx,POCKET_ERR_UNSUPPORTED,"input.onKey",
                            "this host delivers actions, not key events",
                            false,NULL);
}

// Likewise for the text session: it wants the host's IME and edit field, which
// only a screen declaring takes_text receives, and the running-app screen does
// not. input.text keeps pocket_api.c's declared supported=false entry.
static JSValue js_text_open(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    return pocket_api_throw(ctx,POCKET_ERR_UNSUPPORTED,"input.text.open",
                            "no host text session for a running app yet",
                            false,NULL);
}

// -------------------------------------------------------------- pump/reset

void pocket_ui_pump(uint32_t buttons) {
    int64_t now=0;
    if(toast_until) {
        now=esp_timer_get_time();
        if(now>=toast_until && ui_ctx) toast_hide(ui_ctx);
    }
    uint32_t before=held_mask;
    held_mask=buttons;                    // held() works with no listener at all
    if(!action_table.open || !(buttons|before)) return;
    if(!now) now=esp_timer_get_time();
    for(int i=0;i<ACTION_COUNT;i++) {
        uint32_t bit=ACTIONS[i].bit;
        bool down=(buttons&bit)!=0, was=(before&bit)!=0;
        action_event_t event={.action=i,.now=now};
        if(down && !was) {
            event.phase="press";
            repeat_at[i]=now+REPEAT_DELAY_US;
        } else if(!down && was) {
            event.phase="release";
            repeat_at[i]=0;
        } else if(down && now>=repeat_at[i]) {
            event.phase="repeat";
            repeat_at[i]=now+REPEAT_PERIOD_US;
        } else continue;
        pocket_api_sub_deliver(&action_table,action_payload,&event);
    }
}

void pocket_ui_reset(void) {
    JSContext *ctx=ui_ctx;
    pocket_api_sub_close_all(&action_table);
    action_table.ctx=NULL;
    if(ctx) {
        // No node is destroyed here: app_stop() takes the whole core down a few
        // lines later, and the only thing that must not outlive the realm is a
        // reference into it. That is the retained item arrays and the cached
        // binding, and those are what this frees.
        for(int i=0;i<UI_MAX_LISTS;i++)
            if(lists[i].used) JS_FreeValue(ctx,lists[i].items);
        if(ui_ready) {
            for(int i=0;i<F_COUNT;i++) JS_FreeValue(ctx,ui_fn[i]);
            JS_FreeValue(ctx,ui_obj);
        }
        // The originals the wrapper closed over belong to the realm too.
        if(legacy_wrapped) {
            JS_FreeValue(ctx,legacy_create);
            JS_FreeValue(ctx,legacy_destroy);
        }
    }
    // Cleared, not just freed: ui_bind()'s failure path frees ui_fn[0] when
    // globalThis.ui is not an object, and a value left over from a destroyed
    // realm would be freed with the next session's context.
    for(int i=0;i<F_COUNT;i++) ui_fn[i]=JS_UNDEFINED;
    ui_obj=JS_UNDEFINED;
    memset(legacy_ids,0,sizeof(legacy_ids));
    legacy_create=JS_UNDEFINED; legacy_destroy=JS_UNDEFINED;
    legacy_wrapped=false;
    memset(nodes,0,sizeof(nodes));
    memset(lists,0,sizeof(lists));
    memset(screens,0,sizeof(screens));
    memset(repeat_at,0,sizeof(repeat_at));
    depth=0; live_nodes=0; held_mask=0;
    toast_box=0; toast_label=0; toast_until=0;
    ui_ready=false; ui_ctx=NULL;
    // The classes belong to the realm that is going away, so the next session
    // builds its own on the first read of either namespace.
    realm_ready=false;
}

// ------------------------------------------------------------- capabilities

static const pocket_limit_t ui_limits[] = {
    {.name="maxNodes",     .kind=POCKET_LIMIT_INT,  .number=UI_MAX_NODES},
    // Below safeNodes the layout asks the allocator for nothing large and a
    // node is never refused; between it and maxNodes the answer depends on the
    // heap at the time, so a program that must not fail stays under it.
    {.name="safeNodes",    .kind=POCKET_LIMIT_INT,  .number=UI_SAFE_NODES},
    {.name="maxScreens",   .kind=POCKET_LIMIT_INT,  .number=UI_MAX_SCREENS},
    {.name="maxTextBytes", .kind=POCKET_LIMIT_INT,  .number=UI_MAX_TEXT_BYTES},
    {.name="maxGlyphs",    .kind=POCKET_LIMIT_INT,  .number=JSFONT_MAX},
    {.name="maxLists",     .kind=POCKET_LIMIT_INT,  .number=UI_MAX_LISTS},
    {.name="maxListRows",  .kind=POCKET_LIMIT_INT,  .number=UI_LIST_ROWS},
    {.name="maxListItems", .kind=POCKET_LIMIT_INT,  .number=UI_MAX_LIST_ITEMS},
    {.name="maxToastMs",   .kind=POCKET_LIMIT_INT,  .number=UI_TOAST_MAX_MS},
    {.name="fonts",        .kind=POCKET_LIMIT_TEXT, .text="small,body,large"},
    {0},
};

// The honest half of this surface. The pad mask a running app is handed carries
// one button today -- main.c maps Enter to it and consumes every other key for
// the shell -- so accept is the only action that can fire, and a program that
// feature-tests learns it here rather than by waiting for an event that never
// comes. onAction, held() and the repeat pacing are written against the whole
// mask, so the day tick_run() forwards the arrow keys, this line is the only
// thing that has to change.
static const pocket_limit_t input_limits[] = {
    {.name="actions",       .kind=POCKET_LIMIT_TEXT, .text="accept"},
    {.name="maxWatches",    .kind=POCKET_LIMIT_INT,  .number=UI_ACTION_SUBS},
    {.name="repeatDelayMs", .kind=POCKET_LIMIT_INT,  .number=REPEAT_DELAY_US/1000},
    {.name="keyEvents",     .kind=POCKET_LIMIT_FLAG, .number=0},
    {.name="textSessions",  .kind=POCKET_LIMIT_INT,  .number=0},
    {0},
};

static const pocket_capability_t ui_capability = {
    .name="ui.basic", .supported=true, .available=true, .limits=ui_limits,
};
static const pocket_capability_t input_capability = {
    .name="input.action", .supported=true, .available=true, .limits=input_limits,
};

// ------------------------------------------------------------------ install

static const JSCFunctionListEntry screen_methods[] = {
    JS_CFUNC_DEF("text",1,js_screen_text),
    JS_CFUNC_DEF("rect",1,js_screen_rect),
    JS_CFUNC_DEF("list",1,js_screen_list),
    JS_CFUNC_DEF("close",0,js_screen_close),
};
static const JSCFunctionListEntry node_methods[] = {
    JS_CFUNC_DEF("remove",0,js_node_remove),
};
static const JSCFunctionListEntry text_methods[] = {
    JS_CFUNC_DEF("setText",1,js_text_set),
    JS_CFUNC_DEF("setPosition",2,js_text_move),
};
static const JSCFunctionListEntry list_methods[] = {
    JS_CFUNC_DEF("setItems",1,js_list_set_items),
    JS_CFUNC_DEF("select",1,js_list_select),
};

static const JSClassDef screen_def = {.class_name="PocketScreen"};
static const JSClassDef node_def   = {.class_name="PocketNode"};
static const JSClassDef text_def   = {.class_name="PocketTextNode"};
static const JSClassDef list_def   = {.class_name="PocketListNode"};

// None of the four classes has a finalizer, and that is deliberate. A node is
// owned by its screen, not by the wrapper the app happens to be holding: an app
// that drops a label still wants the label drawn. Nothing is freed by the
// collector, so nothing can vanish off the display because a variable went out
// of scope, and the retained item arrays stay alive because the host's own
// reference keeps them so. remove(), close() and pocket_ui_reset() are the
// three ways anything here ends.
//
// Methods live on shared prototypes, so a node costs the guest one object with
// an opaque and nothing else. Per-node closures instead would put two to five
// function objects on every rectangle on the screen, and guest heap -- not
// flash -- is what runs out on this board.
static bool make_class(JSContext *ctx, JSClassID *id, const JSClassDef *def,
                       const JSCFunctionListEntry *methods, int count,
                       JSValueConst parent) {
    JSRuntime *rt=JS_GetRuntime(ctx);
    JS_NewClassID(rt,id);
    if(JS_NewClass(rt,*id,def)<0) return false;
    JSValue proto=JS_IsUndefined(parent)?JS_NewObject(ctx)
                                        :JS_NewObjectProto(ctx,parent);
    if(JS_IsException(proto)) return false;
    JS_SetPropertyFunctionList(ctx,proto,methods,count);
    JS_SetClassProto(ctx,*id,proto);
    return true;
}
#define METHODS(a) (a),(int)(sizeof(a)/sizeof((a)[0]))

// ------------------------------------------- the guard the legacy path needs
//
// budget_ok() only ever saw nodes this surface made, so an app on the raw
// ui.createNode path -- which every app in apps/ still is -- was never asked.
// The firmware held the right ceiling and the pet app walked past it into a
// 29,648-byte contiguous request the heap could not pay, and the Rust side
// aborts rather than failing: the device rebooted instead of the call
// returning an error.
//
// So the two calls that change the tree's size are wrapped, and the counting
// happens in one place for both paths. A refusal is a PocketError the app can
// catch and draw around, which is the whole difference from a reboot.
static JSValue js_legacy_create(JSContext *ctx, JSValueConst this_val,
                                int argc, JSValueConst *argv) {
    if(!budget_ok(ctx,1,"ui.createNode")) return JS_EXCEPTION;
    JSValue result=JS_Call(ctx,legacy_create,this_val,argc,argv);
    if(JS_IsException(result)) return result;
    int32_t id=0;
    if(!JS_ToInt32(ctx,&id,result) && id>0) {
        for(unsigned i=0;i<UI_MAX_NODES;i++)
            if(!legacy_ids[i]) { legacy_ids[i]=id; live_nodes++; break; }
    }
    return result;
}
static JSValue js_legacy_destroy(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    int32_t id=0;
    if(argc>0) JS_ToInt32(ctx,&id,argv[0]);
    JSValue r=JS_Call(ctx,legacy_destroy,this_val,argc,argv);
    // Only an id this wrapper issued and has not yet retired gives the budget
    // anything back. Destroying the root, a dead id or one that never existed
    // costs nothing and returns nothing, so it must credit nothing.
    if(!JS_IsException(r) && id>0)
        for(unsigned i=0;i<UI_MAX_NODES;i++)
            if(legacy_ids[i]==id) {
                legacy_ids[i]=0;
                if(live_nodes) live_nodes--;
                break;
            }
    return r;
}

void pocket_ui_attach(JSContext *ctx) {
    // Not at install: app_session.c mounts the binding after it installs the
    // guest surfaces, so globalThis.ui does not exist yet then. This runs where
    // jsfont.c's setText wrapper does, after the mount and before the app's
    // own source is evaluated.
    if(legacy_wrapped) return;
    JSValue global=JS_GetGlobalObject(ctx);
    JSValue obj=JS_GetPropertyStr(ctx,global,"ui");
    JS_FreeValue(ctx,global);
    if(!JS_IsObject(obj)) { JS_FreeValue(ctx,obj); return; }
    JSValue create=JS_GetPropertyStr(ctx,obj,"createNode");
    JSValue destroy=JS_GetPropertyStr(ctx,obj,"destroyNode");
    if(JS_IsFunction(ctx,create) && JS_IsFunction(ctx,destroy)) {
        legacy_create=create; legacy_destroy=destroy; legacy_wrapped=true;
        JS_SetPropertyStr(ctx,obj,"createNode",
            JS_NewCFunction(ctx,js_legacy_create,"createNode",1));
        JS_SetPropertyStr(ctx,obj,"destroyNode",
            JS_NewCFunction(ctx,js_legacy_destroy,"destroyNode",1));
    } else {
        JS_FreeValue(ctx,create); JS_FreeValue(ctx,destroy);
    }
    JS_FreeValue(ctx,obj);
}

// See realm_ready where it is declared.
static esp_err_t ensure_realm(JSContext *ctx) {
    if(realm_ready) return ESP_OK;
    // Nothing here survives a session: the realm going away takes the callbacks
    // and the retained arrays with it, so every table starts empty.
    memset(nodes,0,sizeof(nodes));
    memset(lists,0,sizeof(lists));
    memset(screens,0,sizeof(screens));
    memset(repeat_at,0,sizeof(repeat_at));
    depth=0; held_mask=0;
    toast_box=0; toast_label=0; toast_until=0;
    for(int i=0;i<UI_ACTION_SUBS;i++) {
        action_slots[i].callback=JS_UNDEFINED;
        action_slots[i].handle=0;
    }
    action_table.open=0;
    action_table.ctx=ctx;

    if(!make_class(ctx,&screen_class,&screen_def,METHODS(screen_methods),
                   JS_UNDEFINED) ||
       !make_class(ctx,&node_class,&node_def,METHODS(node_methods),JS_UNDEFINED))
        return ESP_FAIL;
    // TextNode and ListNode are Nodes: remove() is defined once and inherited,
    // which is both the section 6 type shape and one fewer function object.
    JSValue node_proto=JS_GetClassProto(ctx,node_class);
    bool ok=make_class(ctx,&text_class,&text_def,METHODS(text_methods),node_proto) &&
            make_class(ctx,&list_class,&list_def,METHODS(list_methods),node_proto);
    JS_FreeValue(ctx,node_proto);
    if(!ok) return ESP_FAIL;
    realm_ready=true;
    return ESP_OK;
}

static esp_err_t build_ui(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    esp_err_t err=ensure_realm(ctx);
    if(err!=ESP_OK) return err;
    JS_DefinePropertyValueStr(ctx,ns,"screen",
        JS_NewCFunction(ctx,js_ui_screen,"screen",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"push",
        JS_NewCFunction(ctx,js_ui_push,"push",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"pop",
        JS_NewCFunction(ctx,js_ui_pop,"pop",0),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"toast",
        JS_NewCFunction(ctx,js_ui_toast,"toast",2),JS_PROP_ENUMERABLE);
    return ESP_OK;
}

static esp_err_t build_input(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    esp_err_t err=ensure_realm(ctx);
    if(err!=ESP_OK) return err;
    JS_DefinePropertyValueStr(ctx,ns,"onAction",
        JS_NewCFunction(ctx,js_on_action,"onAction",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"onKey",
        JS_NewCFunction(ctx,js_on_key,"onKey",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"held",
        JS_NewCFunction(ctx,js_held,"held",1),JS_PROP_ENUMERABLE);
    JSValue text=JS_NewObject(ctx);
    if(JS_IsException(text)) return ESP_ERR_NO_MEM;
    JS_DefinePropertyValueStr(ctx,text,"open",
        JS_NewCFunction(ctx,js_text_open,"open",1),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"text",text,JS_PROP_ENUMERABLE);
    return ESP_OK;
}

esp_err_t pocket_ui_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    pocket_api_register(&ui_capability);
    pocket_api_register(&input_capability);

    // Eager, and it has to be: the node budget guards every app, including the
    // ones that never read pocket.ui and build their display with the legacy
    // ui.createNode. pocket_ui_attach() wraps that a few lines after this runs
    // and counts through live_nodes, so the counter and its realm start clean
    // whether or not the namespace is ever built.
    memset(legacy_ids,0,sizeof(legacy_ids));
    live_nodes=0;
    ui_ready=false; ui_ctx=ctx;
    realm_ready=false;

    esp_err_t err=pocket_api_lazy(ctx,"ui",build_ui,NULL);
    if(err!=ESP_OK) return err;
    return pocket_api_lazy(ctx,"input",build_input,NULL);
}
