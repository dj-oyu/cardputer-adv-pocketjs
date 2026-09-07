#include "pocket_workspace.h"
#include "pocket_api.h"
#include "app_registry.h"
#include "srcstore.h"
#include "board.h"
#include "paint.h"
#include "jpfont.h"
#include "utf8.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "pocket.ws";

// ------------------------------------------------------------------- limits
//
// The text limit is srcstore's record size, not a number chosen here: a work is
// a slot, and a slot holds SRC_MAX bytes. Everything else is enforced below
// before anything reaches flash.
#define WS_TEXT_MAX      SRC_MAX
#define WS_TITLE_BYTES   32          // 31 usable, and the picker's row is 31 wide
#define WS_STATE_MAX     1024        // section 7 names this one
#define WS_TIMEOUT_MS    30000       // section 14's general cap
// The picker is the one call in this file that waits for a person rather than
// for flash, so section 14's 30 s would be a deadline on reading a list. Five
// minutes is the cap and two the default; both are refusals of "no deadline at
// all", which would leave a promise that nothing ever settles.
#define WS_PICK_TIMEOUT_MS 300000
#define WS_PICK_DEFAULT_MS 120000

// The works: the Playground's own slot, then srcstore's free tail.
#define WS_ROWS_MAX (SRC_SLOT_WORK_COUNT + 1)

// ------------------------------------------------------------------- the index
//
// Titles are the only thing a work has that srcstore does not already store, so
// they are the only thing here: one NVS blob of 220 bytes holding a title and an
// owner per work slot. Not one record per work — sixteen NVS entries for six
// titles would cost the key/value store its room for the sake of a lookup
// nothing does often.
//
// Nothing is cached. The blob is read onto the stack for the one call that
// needs it and dropped again, because a cache here would be DRAM held for the
// whole run of every app, and the read costs a flash page.
#define WS_INDEX_MAGIC   0x5731U   // 'W','1'
#define WS_INDEX_VERSION 1

typedef struct __attribute__((packed)) {
    uint32_t owner;                  // app_registry_owner_hash, 0 for the host
    char     title[WS_TITLE_BYTES];  // NUL terminated
} ws_entry_t;

typedef struct __attribute__((packed)) {
    uint16_t   magic;
    uint16_t   version;
    ws_entry_t entries[SRC_SLOT_WORK_COUNT];
} ws_index_t;

static void index_blank(ws_index_t *index) {
    memset(index,0,sizeof(*index));
    index->magic=WS_INDEX_MAGIC;
    index->version=WS_INDEX_VERSION;
}

static bool index_open(nvs_handle_t *handle) {
    if(nvs_flash_init()!=ESP_OK) return false;
    return nvs_open("pocketws",NVS_READWRITE,handle)==ESP_OK;
}

// An index that is missing, short or of another version reads as empty rather
// than as an error: the works are in srcstore, and a lost title must not make a
// program unreachable. What it costs is the name, which the picker then shows
// as UNTITLED.
static void index_load(ws_index_t *out) {
    index_blank(out);
    nvs_handle_t handle;
    if(!index_open(&handle)) return;
    size_t size=sizeof(*out);
    ws_index_t read;
    esp_err_t err=nvs_get_blob(handle,"index",&read,&size);
    nvs_close(handle);
    if(err!=ESP_OK || size!=sizeof(*out)) return;
    if(read.magic!=WS_INDEX_MAGIC || read.version!=WS_INDEX_VERSION) return;
    // A title that lost its terminator would run into the next entry.
    for(unsigned i=0;i<SRC_SLOT_WORK_COUNT;i++)
        read.entries[i].title[WS_TITLE_BYTES-1]=0;
    *out=read;
}

static bool index_store(const ws_index_t *index) {
    nvs_handle_t handle;
    if(!index_open(&handle)) return false;
    esp_err_t err=nvs_set_blob(handle,"index",index,sizeof(*index));
    if(err==ESP_OK) err=nvs_commit(handle);
    nvs_close(handle);
    if(err!=ESP_OK) ESP_LOGW(TAG,"index write: %s",esp_err_to_name(err));
    return err==ESP_OK;
}

// ------------------------------------------------------------------ slots

// Which srcstore slots this surface will name. Slot 0 is the Playground's own
// program: it is the person's work like any other, and leaving it out would
// mean the one program on the device an app cannot be handed is the one the
// person actually wrote. It is host-owned, so only the picker can grant it.
static bool slot_is_work(int slot) {
    return slot==SRC_SLOT_USER ||
           (slot>=SRC_SLOT_WORK && slot<SRC_SLOT_WORK+SRC_SLOT_WORK_COUNT);
}

static int index_of(int slot) {
    return slot>=SRC_SLOT_WORK ? slot-SRC_SLOT_WORK : -1;
}

static uint32_t self_hash(void) {
    return app_registry_owner_hash(app_registry_current()->id);
}

static app_works_t self_level(void) { return app_registry_current()->works; }

static bool slot_owned(const ws_index_t *index, int slot) {
    int entry=index_of(slot);
    return entry>=0 && index->entries[entry].owner==self_hash();
}

static void slot_title(const ws_index_t *index, int slot,
                       char out[WS_TITLE_BYTES]) {
    int entry=index_of(slot);
    if(entry<0) { snprintf(out,WS_TITLE_BYTES,"PLAYGROUND"); return; }
    const char *title=index->entries[entry].title;
    snprintf(out,WS_TITLE_BYTES,"%s",title[0]?title:"UNTITLED");
}

// ------------------------------------------------------------------- session

// Which slots the person chose in the picker during this session. A grant, and
// the only thing that turns another app's work into one this app may open, so
// it is a host fact that ends with the session -- section 3's "一度許可した範囲"
// is one range for one run, not a standing permission.
static uint16_t picked_mask;
static JSClassID  ref_class;

static bool slot_picked(int slot) {
    return slot>=0 && slot<16 && (picked_mask&(uint16_t)(1u<<slot));
}

// ------------------------------------------------------------------ SourceRef

static const JSClassDef ref_class_def = { .class_name="PocketSourceRef" };

// The slot lives in the opaque, where JS cannot read it, and the title is put
// on the object because a picker result an app cannot show is a picker result
// an app cannot use. Section 7 asks the reference to be opaque; it does not ask
// the work to be anonymous.
static JSValue ref_make(JSContext *ctx, int slot, const ws_index_t *index) {
    JSValue ref=JS_NewObjectClass(ctx,ref_class);
    if(JS_IsException(ref)) return ref;
    JS_SetOpaque(ref,(void *)(uintptr_t)(slot+1));
    char title[WS_TITLE_BYTES];
    slot_title(index,slot,title);
    JS_DefinePropertyValueStr(ctx,ref,"title",JS_NewString(ctx,title),
                              JS_PROP_ENUMERABLE);
    return ref;
}

// -1 when the value is not a reference this host issued.
static int ref_slot(JSValueConst value) {
    void *opaque=JS_GetOpaque(value,ref_class);
    if(!opaque) return -1;
    int slot=(int)(uintptr_t)opaque-1;
    return slot_is_work(slot)?slot:-1;
}

// ------------------------------------------------------------------- options

typedef struct {
    int64_t if_revision;   // -1 for "not given"
    bool    cancelled;
    int32_t timeout_ms;    // 0 for "not given"
} ws_options_t;

static JSValue take_options(JSContext *ctx, JSValueConst value, const char *op,
                            int32_t max_timeout_ms, ws_options_t *out) {
    out->if_revision=-1;
    out->cancelled=false;
    out->timeout_ms=0;
    if(JS_IsUndefined(value) || JS_IsNull(value)) return JS_UNDEFINED;
    if(!JS_IsObject(value))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                 "options must be an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    JSValue timeout=JS_GetPropertyStr(ctx,value,"timeoutMs");
    if(JS_IsException(timeout)) return JS_EXCEPTION;
    if(!JS_IsUndefined(timeout) && !JS_IsNull(timeout)) {
        double ms=0;
        bool bad=!JS_IsNumber(timeout) || JS_ToFloat64(ctx,&ms,timeout);
        JS_FreeValue(ctx,timeout);
        // Section 4 refuses to round an out-of-range request quietly.
        if(bad || !isfinite(ms) || ms!=(double)(int64_t)ms ||
           ms<1 || ms>max_timeout_ms)
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                     "timeoutMs is out of range for this call",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        out->timeout_ms=(int32_t)ms;
    } else JS_FreeValue(ctx,timeout);

    JSValue cancel=JS_GetPropertyStr(ctx,value,"cancel");
    if(JS_IsException(cancel)) return JS_EXCEPTION;
    if(!JS_IsUndefined(cancel) && !JS_IsNull(cancel)) {
        if(!pocket_api_is_cancel_token(cancel)) {
            JS_FreeValue(ctx,cancel);
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                     "cancel must be a token from pocket.cancel.source()",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        }
        out->cancelled=pocket_api_cancel_requested(cancel);
    }
    JS_FreeValue(ctx,cancel);

    JSValue revision=JS_GetPropertyStr(ctx,value,"ifRevision");
    if(JS_IsException(revision)) return JS_EXCEPTION;
    if(!JS_IsUndefined(revision) && !JS_IsNull(revision)) {
        double r=0;
        bool bad=!JS_IsNumber(revision) || JS_ToFloat64(ctx,&r,revision);
        JS_FreeValue(ctx,revision);
        if(bad || !isfinite(r) || r!=(double)(int64_t)r || r<0 || r>UINT32_MAX)
            return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                     "ifRevision must be a revision number or 0",
                                     false,POCKET_OUTCOME_NOT_APPLIED);
        out->if_revision=(int64_t)r;
    } else JS_FreeValue(ctx,revision);
    return JS_UNDEFINED;
}

// The cancel token JS passed, for the calls that arm a promise with it. Kept
// separate from take_options because only pick() has anything to wait on.
static JSValue take_cancel(JSContext *ctx, JSValueConst options) {
    if(!JS_IsObject(options)) return JS_UNDEFINED;
    JSValue cancel=JS_GetPropertyStr(ctx,options,"cancel");
    if(JS_IsException(cancel) || !pocket_api_is_cancel_token(cancel)) {
        JS_FreeValue(ctx,cancel);
        return JS_UNDEFINED;
    }
    return cancel;
}

// Resolves a SourceRef argument to a slot this app may act on, or builds the
// rejection. `op_kind` is the permission being asked for.
static JSValue take_ref(JSContext *ctx, JSValueConst value, const char *op,
                        app_work_op_t op_kind, const ws_index_t *index,
                        int *slot_out) {
    int slot=ref_slot(value);
    if(slot<0)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                 "ref must be a reference from pocket.workspace",
                                 false,POCKET_OUTCOME_NOT_APPLIED);
    if(!app_registry_may_work(self_level(),op_kind,slot_owned(index,slot),
                              slot_picked(slot)))
        return pocket_api_reject(ctx,POCKET_ERR_PERMISSION_DENIED,op,
                                 "this app may not reach that work",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    *slot_out=slot;
    return JS_UNDEFINED;
}

// A source text argument: the size and encoding rules of section 4, checked
// before anything is erased. Returns the C string (free with JS_FreeCString) or
// NULL with `error` holding the rejection.
static const char *take_text(JSContext *ctx, JSValueConst value, const char *op,
                             size_t *length, JSValue *error) {
    *error=JS_UNDEFINED;
    if(!JS_IsString(value)) {
        *error=pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                 "text must be a string",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
        return NULL;
    }
    size_t n=0;
    const char *text=JS_ToCStringLen(ctx,&n,value);
    if(!text) { *error=JS_EXCEPTION; return NULL; }
    if(n>WS_TEXT_MAX) {
        JS_FreeCString(ctx,text);
        *error=pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,op,
                                 "the source is larger than a work slot",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
        return NULL;
    }
    // QuickJS hands back WTF-8 for a lone surrogate, so this catches both
    // section 4 refusals at once.
    if(!utf8_valid(text,n)) {
        JS_FreeCString(ctx,text);
        *error=pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                                 "the source is not well-formed UTF-8",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
        return NULL;
    }
    *length=n;
    return text;
}

// ------------------------------------------------------------------- picker
//
// Allocated when the picker opens and freed when it closes, so an app that
// never calls pick() pays four bytes of pointer for it.

typedef struct {
    unsigned count, cursor, top;
    uint8_t  slots[WS_ROWS_MAX];
    uint32_t revisions[WS_ROWS_MAX];
    char     titles[WS_ROWS_MAX][WS_TITLE_BYTES];
    char     header[WS_TITLE_BYTES];
    int64_t  deadline_us;
    bool     dirty;
} ws_picker_t;

static ws_picker_t     *picker;
static pocket_request_t pick_request;
static int              pick_slot = -1;   // what the person chose

// What the picker reports back, as a completion status.
#define PICK_CHOSE     POCKET_STATUS_OK
#define PICK_CANCELLED 1
#define PICK_EXPIRED   2

#define PICK_ROWS_VISIBLE 6
#define PICK_LINE_H       15
#define PICK_TOP          19

static void picker_close(void) {
    free(picker);
    picker=NULL;
}

// Both ways out of the picker go through here, so the completion is posted
// exactly once and the screen is given back at the same moment.
static void picker_finish(int32_t status, int slot) {
    if(!picker) return;
    pick_slot=slot;
    picker_close();
    pocket_request_t request=pick_request;
    pick_request=0;
    if(request) pocket_api_complete(request,status);
}

bool pocket_workspace_modal(void) { return picker!=NULL; }

void pocket_workspace_modal_key(const keystroke_t *key) {
    if(!picker || !key) return;
    switch(key->nav) {
        case KEY_UP:
            if(picker->cursor) picker->cursor--;
            break;
        case KEY_DOWN:
            if(picker->count && picker->cursor+1<picker->count) picker->cursor++;
            break;
        case KEY_ENTER:
            if(picker->count) {
                int slot=(int)picker->slots[picker->cursor];
                // The choice is the grant: this is the only place a slot the
                // app does not own becomes one it may open.
                if(slot>=0 && slot<16) picked_mask|=(uint16_t)(1u<<slot);
                ESP_LOGI(TAG,"PICKED %u",(unsigned)slot);
                picker_finish(PICK_CHOSE,slot);
                return;
            }
            break;
        case KEY_BACK:
            ESP_LOGI(TAG,"PICK CANCELLED");
            picker_finish(PICK_CANCELLED,-1);
            return;
        default: return;
    }
    if(picker) {
        if(picker->cursor<picker->top) picker->top=picker->cursor;
        if(picker->cursor>=picker->top+PICK_ROWS_VISIBLE)
            picker->top=picker->cursor-PICK_ROWS_VISIBLE+1;
        picker->dirty=true;
    }
}

bool pocket_workspace_modal_dirty(void) {
    if(!picker) return false;
    // pocket_api_pump() does not run while the guest is not ticked, so the
    // deadline that armed this promise would not be noticed until the person
    // closed the picker -- which is to say never, for the case that needs it.
    // The picker keeps its own watch instead.
    if(esp_timer_get_time()>picker->deadline_us) {
        ESP_LOGI(TAG,"PICK EXPIRED");
        picker_finish(PICK_EXPIRED,-1);
        return false;
    }
    return picker->dirty;
}

static void picker_row(uint16_t *strip, int strip_y, int rows, unsigned row,
                       int y, bool selected, uint16_t ink, uint16_t dim) {
    char meta[12];
    snprintf(meta,sizeof meta,"r%lu",(unsigned long)picker->revisions[row]);
    if(selected) {
        paint_fill(2,y-2,LCD_W-4,PICK_LINE_H-2,dim);
        paint_ascii(5,y+1,">",ink);
    }
    const char *title=picker->titles[row];
    size_t len=strlen(title);
    if(jpfont_ready(JPFONT_TEXT))
        jpfont_draw(JPFONT_TEXT,strip,strip_y,rows,14,y-2,title,len,ink);
    else
        paint_ascii(14,y+1,title,ink);
    paint_ascii(LCD_W-34,y+1,meta,selected?ink:dim);
}

void pocket_workspace_modal_draw(void) {
    if(!picker) return;
    picker->dirty=false;
    uint16_t *strip=board_strip();
    uint16_t ink=board_rgb(226,234,244), dim=board_rgb(70,92,120),
             rule=board_rgb(28,44,66), accent=board_rgb(120,200,255),
             back=board_rgb(8,13,22);

    for(int strip_y=0;strip_y<LCD_H;strip_y+=STRIP_H) {
        int rows=LCD_H-strip_y<STRIP_H?LCD_H-strip_y:STRIP_H;
        paint_begin(strip,strip_y,rows);
        for(int i=0;i<LCD_W*rows;i++) strip[i]=back;

        paint_ascii(5,3,"CHOOSE A WORK",accent);
        paint_ascii(LCD_W-6*(int)strlen(picker->header)-5,3,picker->header,dim);
        paint_fill(0,13,LCD_W,1,rule);

        if(!picker->count)
            paint_ascii(14,PICK_TOP+2,"NO WORKS SAVED YET",dim);
        for(unsigned r=0;r<PICK_ROWS_VISIBLE && picker->top+r<picker->count;r++) {
            unsigned row=picker->top+r;
            picker_row(strip,strip_y,rows,row,PICK_TOP+(int)r*PICK_LINE_H,
                       row==picker->cursor,ink,
                       row==picker->cursor?board_rgb(24,48,78):dim);
        }

        paint_fill(0,LCD_H-11,LCD_W,1,rule);
        paint_ascii(5,LCD_H-8,
                    picker->count?"ENTER CHOOSE   ESC CANCEL":"ESC CANCEL",dim);
        ESP_ERROR_CHECK(board_present(strip_y,rows,strip));
    }
}

// ---------------------------------------------------------------------- pick

static JSValue pick_settle(JSContext *ctx, void *user, int32_t status,
                           const char *stop_code, bool *rejected) {
    (void)user;
    *rejected=true;
    if(stop_code)
        return pocket_api_error(ctx,stop_code,"workspace.pick",
                                "the picker was closed before a choice",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    if(status==PICK_EXPIRED)
        return pocket_api_error(ctx,POCKET_ERR_TIMEOUT,"workspace.pick",
                                "nobody chose a work in time",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    *rejected=false;
    // Section 7 types a cancelled pick as null rather than as a failure: the
    // person deciding not to choose is an answer.
    if(status==PICK_CANCELLED || pick_slot<0) return JS_NULL;
    ws_index_t index;
    index_load(&index);
    return ref_make(ctx,pick_slot,&index);
}

// A cancelled token, an expired deadline or a session ending: the screen goes
// back and the completion follows, which is what pocket_api.c waits for.
static void pick_stop(void *user, const char *code) {
    (void)user; (void)code;
    picker_finish(PICK_CANCELLED,-1);
}

static const pocket_promise_ops_t pick_ops = {
    .settle=pick_settle, .stop=pick_stop,
};

static JSValue js_pick(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    (void)this_val;
    static const char OP[]="workspace.pick";
    JSValueConst options=argc>0?argv[0]:JS_UNDEFINED;

    if(!app_registry_may_work(self_level(),APP_WORK_PICK,false,false))
        return pocket_api_reject(ctx,POCKET_ERR_PERMISSION_DENIED,OP,
                                 "this app may not open the works picker",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    // kind is the one selector section 7 gives, and an unknown one is refused
    // rather than treated as "source": a later kind must not silently mean this
    // one on a firmware that predates it.
    if(JS_IsObject(options)) {
        JSValue kind=JS_GetPropertyStr(ctx,options,"kind");
        if(JS_IsException(kind)) return JS_EXCEPTION;
        if(!JS_IsUndefined(kind) && !JS_IsNull(kind)) {
            const char *name=JS_ToCString(ctx,kind);
            bool ok=name && !strcmp(name,"source");
            if(name) JS_FreeCString(ctx,name);
            JS_FreeValue(ctx,kind);
            if(!ok)
                return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                         "kind must be \"source\"",false,
                                         POCKET_OUTCOME_NOT_APPLIED);
        } else JS_FreeValue(ctx,kind);
    }
    ws_options_t parsed;
    JSValue bad=take_options(ctx,options,OP,WS_PICK_TIMEOUT_MS,&parsed);
    if(!JS_IsUndefined(bad)) return bad;
    if(parsed.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the picker opened",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(picker)
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "the picker is already open",true,
                                 POCKET_OUTCOME_NOT_APPLIED);

    ws_picker_t *fresh=calloc(1,sizeof(*fresh));
    if(!fresh)
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no memory for the picker",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    ws_index_t index;
    index_load(&index);
    // An empty slot is not a work. The revision doubles as the existence test,
    // which is why nothing here has to read 8 KB of source to build a list.
    static const int CANDIDATES[WS_ROWS_MAX]={SRC_SLOT_USER,
        SRC_SLOT_WORK+0,SRC_SLOT_WORK+1,SRC_SLOT_WORK+2,
        SRC_SLOT_WORK+3,SRC_SLOT_WORK+4,SRC_SLOT_WORK+5};
    for(unsigned i=0;i<WS_ROWS_MAX;i++) {
        int slot=CANDIDATES[i];
        uint32_t revision=srcstore_revision((unsigned)slot);
        if(!revision) continue;
        fresh->slots[fresh->count]=(uint8_t)slot;
        fresh->revisions[fresh->count]=revision;
        slot_title(&index,slot,fresh->titles[fresh->count]);
        fresh->count++;
    }
    snprintf(fresh->header,sizeof fresh->header,"%s",app_registry_current()->title);
    fresh->dirty=true;
    int32_t ms=parsed.timeout_ms?parsed.timeout_ms:WS_PICK_DEFAULT_MS;
    fresh->deadline_us=esp_timer_get_time()+1000LL*ms;

    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        free(fresh);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    picker=fresh;
    pick_request=request;
    pick_slot=-1;
    // The promise carries the same deadline the picker watches, so a token
    // cancelled from a frame callback still reaches it -- pocket_api_pump()
    // runs again as soon as the picker is gone, and pick_stop() closes it.
    JSValue promise=pocket_api_promise_arm(ctx,request,&pick_ops,NULL,
                                           take_cancel(ctx,options),
                                           fresh->deadline_us);
    if(JS_IsException(promise)) {
        picker_close();
        pick_request=0;
        return promise;
    }
    ESP_LOGI(TAG,"PICK %u works",fresh->count);
    return promise;
}

// -------------------------------------------------------------------- create

static JSValue js_create(JSContext *ctx, JSValueConst this_val,
                         int argc, JSValueConst *argv) {
    (void)this_val;
    static const char OP[]="workspace.create";
    if(!app_registry_may_work(self_level(),APP_WORK_CREATE,false,false))
        return pocket_api_reject(ctx,POCKET_ERR_PERMISSION_DENIED,OP,
                                 "this app may not create works",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValueConst spec=argc>0?argv[0]:JS_UNDEFINED;
    if(!JS_IsObject(spec))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "create takes {title, text}",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    ws_options_t parsed;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,
                             WS_TIMEOUT_MS,&parsed);
    if(!JS_IsUndefined(bad)) return bad;
    if(parsed.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the work was made",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    JSValue title_value=JS_GetPropertyStr(ctx,spec,"title");
    if(JS_IsException(title_value)) return JS_EXCEPTION;
    size_t title_len=0;
    const char *title_raw=JS_IsString(title_value)
        ? JS_ToCStringLen(ctx,&title_len,title_value) : NULL;
    char title[WS_TITLE_BYTES];
    size_t cleaned=title_raw
        ? app_registry_clean_title(title_raw,title_len,title,sizeof title) : 0;
    if(title_raw) JS_FreeCString(ctx,title_raw);
    JS_FreeValue(ctx,title_value);
    if(!cleaned)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "title must be 1 to 31 bytes of printable UTF-8",
                                 false,POCKET_OUTCOME_NOT_APPLIED);

    JSValue text_value=JS_GetPropertyStr(ctx,spec,"text");
    if(JS_IsException(text_value)) return JS_EXCEPTION;
    size_t length=0;
    JSValue error;
    const char *text=take_text(ctx,text_value,OP,&length,&error);
    JS_FreeValue(ctx,text_value);
    if(!text) return error;

    ws_index_t index;
    index_load(&index);
    int slot=-1;
    for(unsigned i=0;i<SRC_SLOT_WORK_COUNT;i++) {
        int candidate=SRC_SLOT_WORK+(int)i;
        // Free means both halves agree it is free: no owner recorded and no
        // record in the store. Either alone would hand out a slot holding
        // someone's program.
        if(index.entries[i].owner==0 && srcstore_revision((unsigned)candidate)==0) {
            slot=candidate;
            break;
        }
    }
    if(slot<0) {
        JS_FreeCString(ctx,text);
        return pocket_api_reject(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                 "the works library is full",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    bool written=srcstore_save((unsigned)slot,text,length);
    JS_FreeCString(ctx,text);
    if(!written)
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,
                                 "the work could not be written",true,
                                 POCKET_OUTCOME_UNKNOWN);
    int entry=index_of(slot);
    index.entries[entry].owner=self_hash();
    snprintf(index.entries[entry].title,WS_TITLE_BYTES,"%s",title);
    if(!index_store(&index)) {
        // A work with no name is one the picker cannot show and the person
        // cannot ask for again, so the slot goes back rather than becoming a
        // program nobody can reach.
        srcstore_clear((unsigned)slot);
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,
                                 "the work's name could not be written",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    ESP_LOGI(TAG,"CREATED %d %u bytes",slot,(unsigned)length);
    JSValue ref=ref_make(ctx,slot,&index);
    if(JS_IsException(ref)) return ref;
    return pocket_api_settled(ctx,ref,false);
}

// ---------------------------------------------------------------------- read

static JSValue js_read(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    (void)this_val;
    static const char OP[]="workspace.read";
    ws_options_t parsed;
    JSValue bad=take_options(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,
                             WS_TIMEOUT_MS,&parsed);
    if(!JS_IsUndefined(bad)) return bad;
    if(parsed.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the read",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    ws_index_t index;
    index_load(&index);
    int slot=-1;
    bad=take_ref(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,APP_WORK_READ,&index,&slot);
    if(!JS_IsUndefined(bad)) return bad;

    uint32_t revision=srcstore_revision((unsigned)slot);
    if(!revision)
        return pocket_api_reject(ctx,POCKET_ERR_NOT_FOUND,OP,
                                 "that work is empty",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    // One slot's worth, on the heap rather than in .bss: this is the largest
    // single allocation the surface makes and it lives for the length of the
    // call. During a run the largest free block is around 23 KiB, so 8 KiB is
    // affordable and 8 KiB held for the whole session would not be.
    char *buffer=malloc(SRC_MAX+1);
    if(!buffer)
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no memory to read the work",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    // Section 17 asks an empty document to be kept, and section 7 asks a broken
    // one to be reported: both read back as zero bytes, so the store is asked
    // which of the two this is rather than the length being made to mean both.
    bool   verified=false;
    size_t length=srcstore_load_checked((unsigned)slot,buffer,&verified);
    if(!verified) {
        free(buffer);
        return pocket_api_reject(ctx,POCKET_ERR_CORRUPT_DATA,OP,
                                 "the work's record does not verify",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    JSValue result=JS_NewObject(ctx);
    if(JS_IsException(result)) { free(buffer); return result; }
    JS_SetPropertyStr(ctx,result,"text",JS_NewStringLen(ctx,buffer,length));
    free(buffer);
    JS_SetPropertyStr(ctx,result,"revision",JS_NewUint32(ctx,revision));
    char title[WS_TITLE_BYTES];
    slot_title(&index,slot,title);
    JS_SetPropertyStr(ctx,result,"title",JS_NewString(ctx,title));
    return pocket_api_settled(ctx,result,false);
}

// ---------------------------------------------------------------------- save

static JSValue js_save(JSContext *ctx, JSValueConst this_val,
                       int argc, JSValueConst *argv) {
    (void)this_val;
    static const char OP[]="workspace.save";
    ws_options_t parsed;
    JSValue bad=take_options(ctx,argc>2?argv[2]:JS_UNDEFINED,OP,
                             WS_TIMEOUT_MS,&parsed);
    if(!JS_IsUndefined(bad)) return bad;
    if(parsed.cancelled)
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the save",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    ws_index_t index;
    index_load(&index);
    int slot=-1;
    bad=take_ref(ctx,argc>0?argv[0]:JS_UNDEFINED,OP,APP_WORK_SAVE,&index,&slot);
    if(!JS_IsUndefined(bad)) return bad;

    // Section 7: a save onto an existing work needs the explicit choice and the
    // revision check. The choice already happened in the picker; this is the
    // other half, and without it a picked work could be overwritten by an app
    // that never read what was in it.
    if(app_registry_save_needs_revision(slot_owned(&index,slot)) &&
       parsed.if_revision<0)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "saving another work needs ifRevision",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    size_t length=0;
    JSValue error;
    const char *text=take_text(ctx,argc>1?argv[1]:JS_UNDEFINED,OP,&length,&error);
    if(!text) return error;

    uint32_t current=srcstore_revision((unsigned)slot);
    if(parsed.if_revision>=0 && (uint32_t)parsed.if_revision!=current) {
        JS_FreeCString(ctx,text);
        return pocket_api_reject(ctx,POCKET_ERR_CONFLICT,OP,
                                 "the work has moved on since it was read",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    // srcstore writes the other face and leaves the previous version in place
    // until the new header lands, which is the atomic replacement section 7
    // requires: a power cut here costs this save, not the work.
    bool written=srcstore_save((unsigned)slot,text,length);
    JS_FreeCString(ctx,text);
    if(!written)
        return pocket_api_reject(ctx,POCKET_ERR_IO_ERROR,OP,
                                 "the work could not be written",true,
                                 POCKET_OUTCOME_UNKNOWN);
    JSValue result=JS_NewObject(ctx);
    if(JS_IsException(result)) return result;
    JS_SetPropertyStr(ctx,result,"revision",
                      JS_NewUint32(ctx,srcstore_revision((unsigned)slot)));
    return pocket_api_settled(ctx,result,false);
}

// ----------------------------------------------------------------------- run
//
// The handoff to the next session. Everything below is host state on purpose:
// it has to outlive the session that asked for it, which is the one thing
// pocket_workspace_reset() must not throw away.

static int    run_slot = -1;      // accepted, not yet started
static char  *run_source;         // borrowed by app_start_source()
static char  *launch_state;       // the returnState the next session reads
static bool   launch_fresh;       // it belongs to a launch, not to this session

static JSValue js_run(JSContext *ctx, JSValueConst this_val,
                      int argc, JSValueConst *argv) {
    (void)this_val;
    static const char OP[]="workspace.run";
    // Section 7 makes run synchronous and says so twice: it validates the
    // reference, the capability and the state, and throws rather than rejecting.
    ws_index_t index;
    index_load(&index);
    int slot=ref_slot(argc>0?argv[0]:JS_UNDEFINED);
    if(slot<0)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                "ref must be a reference from pocket.workspace",
                                false,POCKET_OUTCOME_NOT_APPLIED);
    if(!app_registry_may_work(self_level(),APP_WORK_RUN,slot_owned(&index,slot),
                              slot_picked(slot)))
        return pocket_api_throw(ctx,POCKET_ERR_PERMISSION_DENIED,OP,
                                "this app may not run that work",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    if(!srcstore_revision((unsigned)slot))
        return pocket_api_throw(ctx,POCKET_ERR_NOT_FOUND,OP,
                                "that work is empty",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    if(run_slot>=0)
        return pocket_api_throw(ctx,POCKET_ERR_BUSY,OP,
                                "a run has already been accepted",false,
                                POCKET_OUTCOME_NOT_APPLIED);

    char *state=NULL;
    if(argc>1 && JS_IsObject(argv[1])) {
        JSValue value=JS_GetPropertyStr(ctx,argv[1],"returnState");
        if(JS_IsException(value)) return JS_EXCEPTION;
        if(!JS_IsUndefined(value) && !JS_IsNull(value)) {
            JSValue json=JS_JSONStringify(ctx,value,JS_UNDEFINED,JS_UNDEFINED);
            JS_FreeValue(ctx,value);
            // A function, a cycle or a BigInt: section 7 wants JSON-compatible
            // and this is where that is decided.
            if(JS_IsException(json)) {
                JS_FreeValue(ctx,JS_GetException(ctx));
                return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                        "returnState must be JSON compatible",
                                        false,POCKET_OUTCOME_NOT_APPLIED);
            }
            size_t n=0;
            const char *text=JS_IsString(json)?JS_ToCStringLen(ctx,&n,json):NULL;
            JS_FreeValue(ctx,json);
            if(!text)
                return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                        "returnState must be JSON compatible",
                                        false,POCKET_OUTCOME_NOT_APPLIED);
            if(n>WS_STATE_MAX) {
                JS_FreeCString(ctx,text);
                return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,OP,
                                        "returnState is over 1024 bytes",false,
                                        POCKET_OUTCOME_NOT_APPLIED);
            }
            state=malloc(n+1);
            if(!state) {
                JS_FreeCString(ctx,text);
                return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                        "no memory for returnState",true,
                                        POCKET_OUTCOME_NOT_APPLIED);
            }
            memcpy(state,text,n);
            state[n]=0;
            JS_FreeCString(ctx,text);
        } else JS_FreeValue(ctx,value);
    }
    free(launch_state);
    launch_state=state;
    launch_fresh=true;
    run_slot=slot;
    ESP_LOGI(TAG,"RUN %d",slot);
    return JS_UNDEFINED;
}

bool pocket_workspace_run_requested(void) { return run_slot>=0; }

bool pocket_workspace_run_take(const char **source, size_t *length) {
    if(run_slot<0) return false;
    int slot=run_slot;
    run_slot=-1;
    free(run_source);
    run_source=malloc(SRC_MAX+1);
    if(!run_source) {
        ESP_LOGW(TAG,"no memory to load work %d",slot);
        return false;
    }
    bool   verified=false;
    size_t n=srcstore_load_checked((unsigned)slot,run_source,&verified);
    if(!verified) {
        // The work verified when run() accepted it and does not now. Nothing to
        // start; the shell stays where it is.
        ESP_LOGW(TAG,"work %d would not load",slot);
        free(run_source);
        run_source=NULL;
        return false;
    }
    *source=run_source;
    *length=n;
    return true;
}

void pocket_workspace_run_done(void) {
    free(run_source);
    run_source=NULL;
}

// -------------------------------------------------------------- launchContext

static JSValue js_launch_context(JSContext *ctx, JSValueConst this_val,
                                 int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    JSValue out=JS_NewObject(ctx);
    if(JS_IsException(out)) return out;
    JSValue state=JS_NULL;
    if(launch_state) {
        state=JS_ParseJSON(ctx,launch_state,strlen(launch_state),"returnState");
        if(JS_IsException(state)) { JS_FreeValue(ctx,JS_GetException(ctx)); state=JS_NULL; }
    }
    JS_SetPropertyStr(ctx,out,"returnState",state);
    // Section 7's other half -- the result of a run this session started -- has
    // no source yet: this host does not bring a caller back, so a result here
    // would be a value nothing ever writes. null is the honest answer, and it
    // is the answer a normal start gives too.
    JS_SetPropertyStr(ctx,out,"result",JS_NULL);
    return out;
}

// The registration record of section 3, read-only, for an app that wants to
// know what it was started as. Built inside the call rather than at namespace
// time: an app asks this once, if ever.
static JSValue js_app_info(JSContext *ctx, JSValueConst this_val,
                           int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    const app_manifest_t *m=app_registry_current();
    JSValue out=JS_NewObject(ctx);
    if(JS_IsException(out)) return out;
    JS_SetPropertyStr(ctx,out,"id",JS_NewString(ctx,m->id));
    JS_SetPropertyStr(ctx,out,"title",JS_NewString(ctx,m->title));
    JS_SetPropertyStr(ctx,out,"entry",JS_NewString(ctx,m->entry));
    JS_SetPropertyStr(ctx,out,"runtime",
        JS_NewString(ctx,m->runtime==APP_RUNTIME_POCKET?"pocket-app":"legacy-pocketjs"));
    JS_SetPropertyStr(ctx,out,"api",m->api?JS_NewString(ctx,m->api):JS_NULL);
    for(int which=0;which<2;which++) {
        const char *const *names=which?m->optional:m->required;
        JSValue array=JS_NewArray(ctx);
        uint32_t n=0;
        if(names) for(;*names;names++)
            JS_SetPropertyUint32(ctx,array,n++,JS_NewString(ctx,*names));
        JS_SetPropertyStr(ctx,out,which?"optional":"required",array);
    }
    JSValue access=JS_NewObject(ctx);
    JS_SetPropertyStr(ctx,access,"works",
        JS_NewString(ctx,m->works==APP_WORKS_PICK?"pick":
                         m->works==APP_WORKS_SELF?"self":"none"));
    JS_SetPropertyStr(ctx,out,"access",access);
    return out;
}

// ------------------------------------------------------------------ capability

static const pocket_limit_t workspace_limits[] = {
    {.name="maxWorks",           .kind=POCKET_LIMIT_INT, .number=SRC_SLOT_WORK_COUNT},
    {.name="maxTextBytes",       .kind=POCKET_LIMIT_INT, .number=WS_TEXT_MAX},
    {.name="maxTitleBytes",      .kind=POCKET_LIMIT_INT, .number=WS_TITLE_BYTES-1},
    {.name="maxReturnStateBytes",.kind=POCKET_LIMIT_INT, .number=WS_STATE_MAX},
    {.name="maxPickTimeoutMs",   .kind=POCKET_LIMIT_INT, .number=WS_PICK_TIMEOUT_MS},
    {.name="maxTimeoutMs",       .kind=POCKET_LIMIT_INT, .number=WS_TIMEOUT_MS},
    {.name="picker",             .kind=POCKET_LIMIT_FLAG,.number=1},
    {.kind=POCKET_LIMIT_END},
};

// available is an observation of the device, not of this app's grant: section 2
// keeps authorisation separate and warns that available=true is not permission.
// The store is there for every app; what differs is what each may ask of it,
// and pocket.app.info().access.works is where an app reads that.
static const pocket_capability_t workspace_capability = {
    .name="workspace", .supported=true, .available=true,
    .limits=workspace_limits,
};

// ---------------------------------------------------------------------- reset

void pocket_workspace_reset(void) {
    // A picker still up when the session ends: the screen goes back, and the
    // completion lands in a slot pocket_api_reset() is about to release.
    picker_finish(PICK_CANCELLED,-1);
    picked_mask=0;
    pick_slot=-1;
    // The launch state belongs to the session that is about to start, not to
    // the one ending, so it survives exactly one reset -- the one run() caused.
    if(launch_fresh) launch_fresh=false;
    else { free(launch_state); launch_state=NULL; }
}

// -------------------------------------------------------------------- install

static const JSCFunctionListEntry workspace_methods[] = {
    JS_CFUNC_DEF("pick",   1, js_pick),
    JS_CFUNC_DEF("read",   2, js_read),
    JS_CFUNC_DEF("create", 2, js_create),
    JS_CFUNC_DEF("save",   3, js_save),
    JS_CFUNC_DEF("run",    2, js_run),
};

// No finalizer on a SourceRef: it holds a slot number and owns nothing, so
// there is nothing for the collector to give back.
static esp_err_t build_workspace(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    JSRuntime *rt=JS_GetRuntime(ctx);
    JS_NewClassID(rt,&ref_class);
    if(JS_NewClass(rt,ref_class,&ref_class_def)<0) return ESP_FAIL;
    JS_SetPropertyFunctionList(ctx,ns,workspace_methods,
        (int)(sizeof(workspace_methods)/sizeof(workspace_methods[0])));
    return ESP_OK;
}

// pocket.app is pocket_app.c's namespace; these two belong beside start() and
// exit() because sections 5 and 7 put them there, and contributing to it is not
// editing that file.
static esp_err_t build_app_extras(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    JS_DefinePropertyValueStr(ctx,ns,"launchContext",
        JS_NewCFunction(ctx,js_launch_context,"launchContext",0),JS_PROP_ENUMERABLE);
    JS_DefinePropertyValueStr(ctx,ns,"info",
        JS_NewCFunction(ctx,js_app_info,"info",0),JS_PROP_ENUMERABLE);
    return ESP_OK;
}

esp_err_t pocket_workspace_install(JSContext *ctx, void *user_data) {
    (void)user_data;
    pocket_api_register(&workspace_capability);
    // A realm going away takes nothing of this with it, so the session state is
    // cleared here rather than trusted to have been.
    picker_close();
    pick_request=0;
    pick_slot=-1;
    picked_mask=0;
    esp_err_t err=pocket_api_lazy(ctx,"workspace",build_workspace,NULL);
    if(err!=ESP_OK) return err;
    return pocket_api_lazy(ctx,"app",build_app_extras,NULL);
}
