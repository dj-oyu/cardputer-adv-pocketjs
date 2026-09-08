// The grant picker for fs.volume.sd. See sd_picker.h for what it is; this file
// is what it costs.
//
// Three things here are decisions rather than mechanics, and each has a reason
// that is invisible from the code:
//
// 1. THE APP NAMES NO FOLDER. requestFolder() takes a volume id and nothing
//    else. Every candidate row comes from readdir() on the card's root, so
//    there is no string an app can supply that reaches sd_media_grant() -- and
//    sd_media_grant() refuses separators anyway, because a defence that only
//    works when its caller is correct is not a defence (sd_path.h).
//
// 2. THE MOUNT IS THE PICKER'S, NOT THE APP'S. Nothing else in this firmware
//    mounts the card, so before the person has been asked, sd_media()->state is
//    ABSENT for every app whether a card is in the slot or not. An app cannot
//    learn from fs.volumes() whether the person has a card inserted, because
//    nothing has looked. That is why opening this screen is the only place
//    sd_media_mount() is called, and it is deliberate rather than incidental:
//    it closes sd_path.h's first invariant at a second level. The error
//    ordering keeps an ungranted app from learning what is there from WHICH
//    refusal it got; this keeps it from learning anything at all, because
//    there is nothing to learn until a person has answered. Do not add a
//    mount anywhere else -- a probe, a boot-time scan, a "is a card in?"
//    helper -- without deciding what it tells an app that never asked.
//
// 3. A CANCELLED PICK UNMOUNTS AGAIN. A mount is 7,032 bytes of heap (measured
//    2026-09-08) on a board where the guest's room is the scarce thing. If the
//    person said no, nobody is holding those bytes -- but only if no earlier
//    grant is live, because unmounting drops the grant and would silently
//    invalidate handles the person did authorise.
#include "sd_picker.h"
#include "sd_media.h"
#include "pocket_api.h"
#include "board.h"
#include "paint.h"
#include "jpfont.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "pocket.sd";

// A row's name. SD_ROOT_MAX bounds what a grant can hold; a name longer than
// this cannot become a root, so it is not offered rather than offered and then
// refused at the moment of choosing.
#define SD_PICK_NAME     64
#define SD_PICK_ROWS_MAX 12
#define SD_PICK_ROWS_VISIBLE 6
#define SD_PICK_LINE_H   15
#define SD_PICK_TOP      19

// The picker waits for a person, so section 8's 30 s general cap would be a
// deadline on reading a list. The same two numbers the works picker uses, for
// the same reason: a refusal of "no deadline at all", which would leave a
// promise nothing ever settles.
#define SD_PICK_TIMEOUT_MS 300000
#define SD_PICK_DEFAULT_MS 120000

#define PICK_CHOSE     POCKET_STATUS_OK
#define PICK_CANCELLED 1
#define PICK_EXPIRED   2

// Allocated when the screen opens and freed when it closes, so an app that
// never asks for a folder pays a pointer for it.
typedef struct {
    unsigned count, cursor, top;
    char     names[SD_PICK_ROWS_MAX][SD_PICK_NAME];
    int64_t  deadline_us;
    bool     dirty;
    // Whether a grant was already live when this screen opened. It decides
    // whether a cancel may unmount: dropping the card would take that grant
    // with it, which is right for a removal and wrong for "the person changed
    // their mind about changing their mind".
    bool     had_grant;
} sd_picker_t;

static sd_picker_t     *picker;
static pocket_request_t pick_request;
static bool             pick_granted;   // what the person did, for the settle

static void picker_close(void) {
    free(picker);
    picker=NULL;
}

// Both ways out go through here, so the completion is posted exactly once and
// the screen is given back at the same moment.
static void picker_finish(int32_t status, bool granted) {
    if(!picker) return;
    bool unmount = !granted && !picker->had_grant;
    pick_granted=granted;
    picker_close();
    // Give the 7 KB back when nothing came of it. sd_media_unmount() revokes as
    // it goes, which is exactly right here: there was nothing to revoke.
    if(unmount) sd_media_unmount();
    pocket_request_t request=pick_request;
    pick_request=0;
    if(request) pocket_api_complete(request,status);
}

bool sd_picker_modal(void) { return picker!=NULL; }

void sd_picker_modal_key(const keystroke_t *key) {
    if(!picker||!key) return;
    switch(key->nav) {
        case KEY_UP:
            if(picker->cursor) picker->cursor--;
            break;
        case KEY_DOWN:
            if(picker->count&&picker->cursor+1<picker->count) picker->cursor++;
            break;
        case KEY_ENTER:
            if(picker->count) {
                const char *folder=picker->names[picker->cursor];
                // The choice is the grant, and this is the only call to
                // sd_media_grant() in the firmware.
                if(!sd_media_grant_folder(folder,strlen(folder))) {
                    // Only a name this screen should not have offered can land
                    // here. Refusing loudly beats granting something else.
                    ESP_LOGW(TAG,"grant refused for \"%s\"",folder);
                    picker_finish(PICK_CANCELLED,false);
                    return;
                }
                ESP_LOGI(TAG,"GRANTED %s",folder);
                picker_finish(PICK_CHOSE,true);
                return;
            }
            break;
        case KEY_BACK:
            ESP_LOGI(TAG,"GRANT DECLINED");
            picker_finish(PICK_CANCELLED,false);
            return;
        default: return;
    }
    if(picker) {
        if(picker->cursor<picker->top) picker->top=picker->cursor;
        if(picker->cursor>=picker->top+SD_PICK_ROWS_VISIBLE)
            picker->top=picker->cursor-SD_PICK_ROWS_VISIBLE+1;
        picker->dirty=true;
    }
}

bool sd_picker_modal_dirty(void) {
    if(!picker) return false;
    // pocket_api_pump() does not run while the guest is not ticked, so the
    // deadline that armed this promise would not be noticed until the person
    // closed the screen -- which is to say never, for the case that needs it.
    if(esp_timer_get_time()>picker->deadline_us) {
        ESP_LOGI(TAG,"GRANT EXPIRED");
        picker_finish(PICK_EXPIRED,false);
        return false;
    }
    return picker->dirty;
}

static void picker_row(uint16_t *strip, int strip_y, int rows, unsigned row,
                       int y, bool selected, uint16_t ink, uint16_t dim) {
    if(selected) {
        paint_fill(2,y-2,LCD_W-4,SD_PICK_LINE_H-2,dim);
        paint_ascii(5,y+1,">",ink);
    }
    const char *name=picker->names[row];
    size_t len=strlen(name);
    if(jpfont_ready(JPFONT_TEXT))
        jpfont_draw(JPFONT_TEXT,strip,strip_y,rows,14,y-2,name,len,ink);
    else
        paint_ascii(14,y+1,name,ink);
}

void sd_picker_modal_draw(void) {
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

        paint_ascii(5,3,"SHARE A FOLDER ON THE CARD",accent);
        paint_fill(0,13,LCD_W,1,rule);

        if(!picker->count)
            paint_ascii(14,SD_PICK_TOP+2,"NO FOLDERS IN THE CARD ROOT",dim);
        for(unsigned r=0;r<SD_PICK_ROWS_VISIBLE&&picker->top+r<picker->count;r++) {
            unsigned row=picker->top+r;
            picker_row(strip,strip_y,rows,row,SD_PICK_TOP+(int)r*SD_PICK_LINE_H,
                       row==picker->cursor,ink,
                       row==picker->cursor?board_rgb(24,48,78):dim);
        }

        paint_fill(0,LCD_H-11,LCD_W,1,rule);
        paint_ascii(5,LCD_H-8,
                    picker->count?"ENTER SHARE   ESC DECLINE":"ESC DECLINE",dim);
        ESP_ERROR_CHECK(board_present(strip_y,rows,strip));
    }
}

// ------------------------------------------------------------------ the call

static JSValue pick_settle(JSContext *ctx, void *user, int32_t status,
                           const char *stop_code, bool *rejected) {
    (void)user;
    *rejected=true;
    if(stop_code)
        return pocket_api_error(ctx,stop_code,"fs.requestFolder",
                                "the folder screen closed before a choice",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    if(status==PICK_EXPIRED)
        return pocket_api_error(ctx,POCKET_ERR_TIMEOUT,"fs.requestFolder",
                                "nobody chose a folder in time",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    *rejected=false;
    // A person declining is an answer, not a failure -- the same typing
    // workspace.pick() gives a cancelled pick.
    if(status!=PICK_CHOSE||!pick_granted) return JS_NULL;
    // The virtual root, not the folder's name on the card. Everything the app
    // may reach is addressed through this prefix, and the name it was cut from
    // is the card's business: the person saw it on the host screen, which is
    // where a name belongs.
    return JS_NewString(ctx,"sd:/");
}

static void pick_stop(void *user, const char *code) {
    (void)user; (void)code;
    picker_finish(PICK_CANCELLED,false);
}

static const pocket_promise_ops_t pick_ops = {
    .settle=pick_settle, .stop=pick_stop,
};

// The card's root directory, one page of folders deep. Files are skipped: a
// grant is a folder, so a row that could not become one is not a row.
static unsigned collect_folders(sd_picker_t *out) {
    DIR *d=opendir(SD_MOUNT_POINT);
    if(!d) {
        sd_media_note_error(ESP_ERR_TIMEOUT);
        return 0;
    }
    struct dirent *de;
    char full[SD_FSPATH_MAX];
    while((de=readdir(d))!=NULL&&out->count<SD_PICK_ROWS_MAX) {
        size_t n=strlen(de->d_name);
        if(!n||de->d_name[0]=='.') continue;      // "." ".." and hidden
        if(n>=SD_PICK_NAME) continue;             // cannot become a root
        int w=snprintf(full,sizeof full,"%s/%s",SD_MOUNT_POINT,de->d_name);
        if(w<0||(size_t)w>=sizeof full) continue;
        struct stat st;
        if(stat(full,&st)!=0||!S_ISDIR(st.st_mode)) continue;
        memcpy(out->names[out->count],de->d_name,n+1);
        out->count++;
    }
    closedir(d);
    return out->count;
}

// timeoutMs and cancel, which are the only two options this call has anything
// to do with. Everything else in Options belongs to a call that waits on the
// medium, and this one waits on a person.
static bool take_timeout(JSContext *ctx, JSValueConst options, const char *op,
                         int32_t *out, JSValue *bad) {
    *out=SD_PICK_DEFAULT_MS;
    if(!JS_IsObject(options)) return true;
    JSValue v=JS_GetPropertyStr(ctx,options,"timeoutMs");
    if(JS_IsException(v)) { *bad=JS_EXCEPTION; return false; }
    if(JS_IsUndefined(v)||JS_IsNull(v)) { JS_FreeValue(ctx,v); return true; }
    double ms=0;
    bool ok=JS_IsNumber(v)&&!JS_ToFloat64(ctx,&ms,v)&&ms==(double)(int64_t)ms&&
            ms>=1&&ms<=SD_PICK_TIMEOUT_MS;
    JS_FreeValue(ctx,v);
    if(!ok) {
        *bad=pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                               "timeoutMs must be a whole number of 1 to 300000",
                               false,POCKET_OUTCOME_NOT_APPLIED);
        return false;
    }
    *out=(int32_t)ms;
    return true;
}

static JSValue take_cancel(JSContext *ctx, JSValueConst options) {
    if(!JS_IsObject(options)) return JS_UNDEFINED;
    JSValue cancel=JS_GetPropertyStr(ctx,options,"cancel");
    if(JS_IsException(cancel)||!pocket_api_is_cancel_token(cancel)) {
        JS_FreeValue(ctx,cancel);
        return JS_UNDEFINED;
    }
    return cancel;
}

JSValue sd_picker_request(JSContext *ctx, JSValueConst self,
                          int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.requestFolder";
    // The volume id is named rather than assumed: app: and assets: need no
    // permission and have no folder to choose, and a future removable volume
    // must not silently mean this one on a firmware that predates it.
    const char *id=argc>0&&JS_IsString(argv[0])?JS_ToCString(ctx,argv[0]):NULL;
    bool is_sd=id&&(!strcmp(id,"sd")||!strcmp(id,"sd:/")||!strcmp(id,"sd:"));
    if(id) JS_FreeCString(ctx,id);
    if(!is_sd)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "only the sd volume has a folder to grant",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValueConst options=argc>1?argv[1]:JS_UNDEFINED;
    if(!JS_IsUndefined(options)&&!JS_IsNull(options)&&!JS_IsObject(options))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "options must be an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValue bad=JS_UNDEFINED;
    int32_t ms=0;
    if(!take_timeout(ctx,options,OP,&ms,&bad)) return bad;
    JSValue cancel=take_cancel(ctx,options);
    if(pocket_api_cancel_requested(cancel)) {
        JS_FreeValue(ctx,cancel);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the screen opened",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(picker) {
        JS_FreeValue(ctx,cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "the folder screen is already open",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }

    sd_picker_t *fresh=calloc(1,sizeof(*fresh));
    if(!fresh) {
        JS_FreeValue(ctx,cancel);
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no memory for the folder screen",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    fresh->had_grant=sd_media()->granted;
    // The one mount in this firmware. Before this line, no app can tell a card
    // in the slot from an empty one, because nothing has asked the hardware.
    if(!sd_media_mount()) {
        free(fresh);
        JS_FreeValue(ctx,cancel);
        return pocket_api_reject(ctx,POCKET_ERR_DISCONNECTED,OP,
                                 "no card could be mounted",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    collect_folders(fresh);
    fresh->dirty=true;
    fresh->deadline_us=esp_timer_get_time()+1000LL*ms;

    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        if(!fresh->had_grant) sd_media_unmount();
        free(fresh);
        JS_FreeValue(ctx,cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    picker=fresh;
    pick_request=request;
    pick_granted=false;
    JSValue promise=pocket_api_promise_arm(ctx,request,&pick_ops,NULL,cancel,
                                           fresh->deadline_us);
    if(JS_IsException(promise)) {
        bool unmount=!fresh->had_grant;
        picker_close();
        pick_request=0;
        if(unmount) sd_media_unmount();
        return promise;
    }
    ESP_LOGI(TAG,"PICK %u folders",fresh->count);
    return promise;
}

void sd_picker_reset(void) {
    // A screen still up when the session ends: the display goes back and the
    // completion follows, which is what pocket_api_reset() waits for.
    picker_finish(PICK_CANCELLED,false);
    pick_request=0;
    pick_granted=false;
}
