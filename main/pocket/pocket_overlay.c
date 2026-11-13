#include "pocket_overlay.h"
#include "pocket_api.h"
#include <string.h>
#include <stdio.h>

// Drawing is not done here: since api-reorg P5 an overlay draws through the
// region-scoped pocket.kasane APP endpoint. The display-list calls
// (overlay.begin/rect/text) and the 24-item list behind them (1,440 B of
// .bss) were removed; this file keeps the region, the key listeners and the
// capability.
static overlay_region_t region;
static bool            have_region;

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
    region=*r; have_region=true;
}
void pocket_overlay_reset(void) {
    pocket_api_sub_close_all(&key_table);
    key_table.ctx=NULL;
    key_head=key_tail=0;
    have_region=false;
    memset(&region,0,sizeof region);
}

// ------------------------------------------------------------- capability

static pocket_limit_t overlay_limits[] = {
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
