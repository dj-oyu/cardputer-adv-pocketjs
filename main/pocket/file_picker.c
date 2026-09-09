#include "file_picker.h"
#include "pickmodal.h"
#include "sd_media.h"
#include "sd_path.h"
#include "pocket_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG="pocket.pick";

// The same two numbers the folder and works pickers use, for the same reason:
// a refusal of "no deadline at all", which would leave a promise nothing ever
// settles, without putting section 8's 30 s cap on a person reading a list.
#define PICK_TIMEOUT_MS 300000
#define PICK_DEFAULT_MS 120000

#define PICK_EXT_MAX     8   // extensions an app may filter on
#define PICK_EXT_LEN     8   // ".flac" and shorter; longer is not a suffix here
#define PICK_DEPTH_MAX   4   // folders deep below the grant root
// The virtual path under sd:/, without the prefix. Chosen so that the LONGEST
// path this screen can hand back -- "sd:/" + this + a separator + a name --
// stays under pocket_fs.c's FS_MAX_PATH of 256. A picker that returns a path
// fs.open() then refuses would be worse than one that never offered the file.
#define PICK_REL_MAX   160

#define PICK_CHOSE     POCKET_STATUS_OK
#define PICK_CANCELLED 1
#define PICK_EXPIRED   2

typedef struct {
    pickmodal_t modal;
    char        ext[PICK_EXT_MAX][PICK_EXT_LEN];
    unsigned    ext_count;
    // Where the person has walked to, as the virtual path under sd:/ with no
    // leading or trailing separator. Empty at the grant root.
    char        rel[PICK_REL_MAX];
    unsigned    depth;
    // The generation the screen opened under. A card swapped mid-browse would
    // otherwise let a path chosen from one filesystem be returned against
    // another -- the same reason sd_path.h makes removal drop the grant.
    uint32_t    generation;
} file_picker_t;

static file_picker_t   *picker;
static pocket_request_t pick_request;
static bool             pick_chose;
// The answer outlives the screen: the settle runs on the first turn the guest
// is ticked again, and by then the picker has been freed.
static char             pick_result[4+PICK_REL_MAX+1+PICK_NAME_MAX+1];

// ------------------------------------------------------------------ the list

static bool ext_allowed(const file_picker_t *fp, const char *name, size_t len) {
    if(!fp->ext_count) return true;
    const char *dot=NULL;
    for(size_t i=len;i>0;i--) if(name[i-1]=='.') { dot=name+i-1; break; }
    if(!dot) return false;
    size_t have=(size_t)(name+len-dot);
    for(unsigned e=0;e<fp->ext_count;e++) {
        const char *want=fp->ext[e];
        size_t wl=strlen(want);
        if(wl!=have) continue;
        // FatFs folds case when it matches a long name, so "Track.MP3" and
        // "track.mp3" are one file there. A case-sensitive filter would hide
        // files the volume itself considers the same name.
        size_t i=0;
        for(;i<wl;i++) {
            char a=dot[i],b=want[i];
            if(a>='A'&&a<='Z') a=(char)(a-'A'+'a');
            if(b>='A'&&b<='Z') b=(char)(b-'A'+'a');
            if(a!=b) break;
        }
        if(i==wl) return true;
    }
    return false;
}

// The FatFs path of the directory the person is currently standing in.
static bool here(const file_picker_t *fp, char *out, size_t outsz) {
    return sd_path_build(sd_media(),fp->rel,strlen(fp->rel),out,outsz)
           ==SD_PATH_OK;
}

static unsigned fill(void *user, unsigned from, pick_row_t *out, unsigned max,
                     bool *more) {
    file_picker_t *fp=user;
    *more=false;
    char dirpath[SD_FSPATH_MAX];
    if(!here(fp,dirpath,sizeof dirpath)) return 0;
    // opendir takes one of the FatFs file slots (SD_MAX_OPEN_FILES is 2), so an
    // app holding two open handles makes this fail rather than return an empty
    // folder -- and the screen says "nothing to choose" either way, which is
    // wrong in a way nobody can act on. sd_media.h already records that the
    // slot count is due a revisit; this is one more thing that decides it.
    DIR *d=opendir(dirpath);
    if(!d) { sd_media_note_error(errno); return 0; }
    struct dirent *de;
    char child[SD_FSPATH_MAX];
    unsigned seen=0,emitted=0;
    while((de=readdir(d))!=NULL) {
        size_t n=strlen(de->d_name);
        if(!n||de->d_name[0]=='.') continue;
        if(n>=PICK_NAME_MAX) continue;        // cannot be shown, so not offered
        if(sd_name_reserved(de->d_name,n)) continue;
        int w=snprintf(child,sizeof child,"%s/%s",dirpath,de->d_name);
        if(w<0||(size_t)w>=sizeof child) continue;
        struct stat st;
        if(stat(child,&st)!=0) continue;
        bool dir=S_ISDIR(st.st_mode);
        // A folder at the depth limit is not offered: entering it is the only
        // thing Enter could mean there, and a row that cannot do its one thing
        // is worse than an absent row.
        if(dir&&fp->depth+1>PICK_DEPTH_MAX) continue;
        if(!dir&&!ext_allowed(fp,de->d_name,n)) continue;
        // Counted AFTER the filters, because `from` indexes the rows this
        // function emits. Counting skipped entries into it would make the next
        // window start short and repeat what this one dropped.
        if(seen++<from) continue;
        if(emitted>=max) { *more=true; break; }
        memcpy(out[emitted].name,de->d_name,n+1);
        out[emitted].is_dir=dir;
        out[emitted].size=dir?0u
            :(uint32_t)(st.st_size>0xFFFFFFFF?0xFFFFFFFF:(uint32_t)st.st_size);
        emitted++;
    }
    closedir(d);
    return emitted;
}

// ---------------------------------------------------------------- the screen

static void picker_close(void) {
    free(picker);
    picker=NULL;
}

// Both ways out go through here, so the completion is posted exactly once and
// the screen is given back at the same moment.
static void picker_finish(int32_t status, bool chose) {
    if(!picker) return;
    pick_chose=chose;
    picker_close();
    pocket_request_t request=pick_request;
    pick_request=0;
    if(request) pocket_api_complete(request,status);
}

bool file_picker_modal(void) { return picker!=NULL; }

static void enter_dir(file_picker_t *fp, const char *name) {
    size_t rl=strlen(fp->rel), nl=strlen(name);
    if(rl+(rl?1u:0u)+nl>=sizeof fp->rel) return;
    if(rl) fp->rel[rl++]='/';
    memcpy(fp->rel+rl,name,nl+1);
    fp->depth++;
    pickmodal_reload(&fp->modal);
}

static void leave_dir(file_picker_t *fp) {
    char *slash=strrchr(fp->rel,'/');
    if(slash) *slash='\0';
    else fp->rel[0]='\0';
    fp->depth--;
    pickmodal_reload(&fp->modal);
}

void file_picker_modal_key(const keystroke_t *key) {
    if(!picker||!key) return;
    file_picker_t *fp=picker;
    switch(pickmodal_key(&fp->modal,key)) {
        case PICK_EVENT_CHOSE: {
            const pick_row_t *row=pickmodal_current(&fp->modal);
            if(!row) return;
            if(row->is_dir) { enter_dir(fp,row->name); return; }
            snprintf(pick_result,sizeof pick_result,"sd:/%s%s%s",
                     fp->rel,fp->rel[0]?"/":"",row->name);
            ESP_LOGI(TAG,"PICKED %s",pick_result);
            picker_finish(PICK_CHOSE,true);
            return;
        }
        case PICK_EVENT_CANCELLED:
            // Escape climbs before it cancels. A person three folders in who
            // wants out of the last one should not have to start the whole
            // request again to do it.
            if(fp->depth) { leave_dir(fp); return; }
            ESP_LOGI(TAG,"PICK DECLINED");
            picker_finish(PICK_CANCELLED,false);
            return;
        default:
            return;
    }
}

bool file_picker_modal_dirty(void) {
    if(!picker) return false;
    // pocket_api_pump() does not run while the guest is not ticked, so a
    // deadline that passed while the person read the list would go unnoticed
    // until they closed the screen -- which is to say never, for the case that
    // needs it.
    if(pickmodal_expired(&picker->modal,esp_timer_get_time())) {
        ESP_LOGI(TAG,"PICK EXPIRED");
        picker_finish(PICK_EXPIRED,false);
        return false;
    }
    // The card can leave under a screen that is only reading it. The generation
    // is what notices, because this board has no card-detect pin to notice
    // with, and a path chosen from one filesystem must not come back against
    // another.
    if(!sd_generation_valid(sd_media(),picker->generation)) {
        ESP_LOGI(TAG,"PICK LOST THE CARD");
        picker_finish(PICK_CANCELLED,false);
        return false;
    }
    return picker->modal.dirty;
}

void file_picker_modal_draw(void) {
    if(!picker) return;
    pickmodal_draw(&picker->modal);
}

// ------------------------------------------------------------------ the call

static JSValue pick_settle(JSContext *ctx, void *user, int32_t status,
                           const char *stop_code, bool *rejected) {
    (void)user;
    *rejected=true;
    if(stop_code)
        return pocket_api_error(ctx,stop_code,"fs.pickFile",
                                "the file screen closed before a choice",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    if(status==PICK_EXPIRED)
        return pocket_api_error(ctx,POCKET_ERR_TIMEOUT,"fs.pickFile",
                                "nobody chose a file in time",false,
                                POCKET_OUTCOME_NOT_APPLIED);
    *rejected=false;
    // A person declining is an answer, not a failure -- the same typing
    // requestFolder and workspace.pick() give a cancelled pick.
    if(status!=PICK_CHOSE||!pick_chose) return JS_NULL;
    return JS_NewString(ctx,pick_result);
}

static void pick_stop(void *user, const char *code) {
    (void)user; (void)code;
    picker_finish(PICK_CANCELLED,false);
}

static const pocket_promise_ops_t pick_ops = {
    .settle=pick_settle, .stop=pick_stop,
};

// timeoutMs and cancel, spelled the same way sd_picker.c spells them because
// they mean the same thing: this call waits on a person, not on the medium. If
// a fourth picker appears these two belong in pocket_api.c; two copies is not
// yet enough to earn a place in the substrate.
static bool take_timeout(JSContext *ctx, JSValueConst options, const char *op,
                         int32_t *out, JSValue *bad) {
    *out=PICK_DEFAULT_MS;
    if(!JS_IsObject(options)) return true;
    JSValue v=JS_GetPropertyStr(ctx,options,"timeoutMs");
    if(JS_IsException(v)) { *bad=JS_EXCEPTION; return false; }
    if(JS_IsUndefined(v)||JS_IsNull(v)) { JS_FreeValue(ctx,v); return true; }
    double ms=0;
    bool ok=JS_IsNumber(v)&&!JS_ToFloat64(ctx,&ms,v)&&ms==(double)(int64_t)ms&&
            ms>=1&&ms<=PICK_TIMEOUT_MS;
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

// options.extensions: the app narrows what it is offered, and that is ALL it
// gets to say about the list. It cannot name a folder, cannot set the words on
// the screen, and cannot see a row the person did not choose -- so a filter is
// a convenience for the person, never a query about what is on the card.
static bool take_extensions(JSContext *ctx, JSValueConst options,
                            const char *op, file_picker_t *fp, JSValue *bad) {
    fp->ext_count=0;
    if(!JS_IsObject(options)) return true;
    JSValue v=JS_GetPropertyStr(ctx,options,"extensions");
    if(JS_IsException(v)) { *bad=JS_EXCEPTION; return false; }
    if(JS_IsUndefined(v)||JS_IsNull(v)) { JS_FreeValue(ctx,v); return true; }
    bool ok=JS_IsArray(v);
    uint32_t n=0;
    if(ok) {
        JSValue len=JS_GetPropertyStr(ctx,v,"length");
        ok=!JS_ToUint32(ctx,&n,len);
        JS_FreeValue(ctx,len);
    }
    if(ok&&n>PICK_EXT_MAX) ok=false;
    for(uint32_t i=0;ok&&i<n;i++) {
        JSValue e=JS_GetPropertyUint32(ctx,v,i);
        const char *s=JS_IsString(e)?JS_ToCString(ctx,e):NULL;
        size_t l=s?strlen(s):0;
        // Written with the dot, so ".mp3" cannot also match "remp3". The API
        // asks for the suffix as it appears in the name rather than for a
        // "type", because a type would be a table this firmware would then own.
        if(s&&l>=2&&l<PICK_EXT_LEN&&s[0]=='.')
            memcpy(fp->ext[fp->ext_count++],s,l+1);
        else ok=false;
        if(s) JS_FreeCString(ctx,s);
        JS_FreeValue(ctx,e);
    }
    JS_FreeValue(ctx,v);
    if(!ok) {
        fp->ext_count=0;
        *bad=pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,op,
                               "extensions must be up to 8 strings like \".mp3\"",
                               false,POCKET_OUTCOME_NOT_APPLIED);
        return false;
    }
    return true;
}

JSValue file_picker_request(JSContext *ctx, JSValueConst self,
                            int argc, JSValueConst *argv) {
    (void)self;
    static const char OP[]="fs.pickFile";
    const char *id=argc>0&&JS_IsString(argv[0])?JS_ToCString(ctx,argv[0]):NULL;
    bool is_sd=id&&(!strcmp(id,"sd")||!strcmp(id,"sd:/")||!strcmp(id,"sd:"));
    if(id) JS_FreeCString(ctx,id);
    if(!is_sd)
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "only the sd volume has files to pick",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValueConst options=argc>1?argv[1]:JS_UNDEFINED;
    if(!JS_IsUndefined(options)&&!JS_IsNull(options)&&!JS_IsObject(options))
        return pocket_api_reject(ctx,POCKET_ERR_INVALID_ARGUMENT,OP,
                                 "options must be an object",false,
                                 POCKET_OUTCOME_NOT_APPLIED);

    // GRANT BEFORE MEDIA, and sd_path.h says why at length: swapping these two
    // makes the error an ungranted app receives depend on whether a card is in
    // the slot, which tells it something it was never authorised to learn.
    // This call opens no picker of its own accord -- browsing a card the app
    // was never given would turn a picker into a way around the picker.
    if(!sd_media()->granted)
        return pocket_api_reject(ctx,POCKET_ERR_PERMISSION_DENIED,OP,
                                 "no folder on the card has been shared with "
                                 "this app; call fs.requestFolder first",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    if(sd_media()->state!=SD_MEDIA_READY)
        return pocket_api_reject(ctx,POCKET_ERR_DISCONNECTED,OP,
                                 "the card is not readable",true,
                                 POCKET_OUTCOME_NOT_APPLIED);

    JSValue bad=JS_UNDEFINED;
    int32_t ms=0;
    if(!take_timeout(ctx,options,OP,&ms,&bad)) return bad;
    if(picker)
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "the file screen is already open",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    JSValue cancel=take_cancel(ctx,options);
    if(pocket_api_cancel_requested(cancel)) {
        JS_FreeValue(ctx,cancel);
        return pocket_api_reject(ctx,POCKET_ERR_CANCELLED,OP,
                                 "cancelled before the screen opened",false,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }

    file_picker_t *fresh=calloc(1,sizeof(*fresh));
    if(!fresh) {
        JS_FreeValue(ctx,cancel);
        return pocket_api_reject(ctx,POCKET_ERR_OUT_OF_MEMORY,OP,
                                 "no memory for the file screen",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    if(!take_extensions(ctx,options,OP,fresh,&bad)) {
        free(fresh);
        JS_FreeValue(ctx,cancel);
        return bad;
    }
    fresh->generation=sd_media()->generation;

    pocket_request_t request=pocket_api_promise_open();
    if(!request) {
        free(fresh);
        JS_FreeValue(ctx,cancel);
        return pocket_api_reject(ctx,POCKET_ERR_BUSY,OP,
                                 "too many operations are pending",true,
                                 POCKET_OUTCOME_NOT_APPLIED);
    }
    // Published before the window is filled, because fill() reads it back
    // through the user pointer.
    picker=fresh;
    pick_request=request;
    pick_chose=false;
    pick_result[0]='\0';
    int64_t deadline=esp_timer_get_time()+1000LL*ms;
    pickmodal_cfg_t cfg={
        .title="CHOOSE A FILE ON THE CARD",
        .hint_rows="ENTER CHOOSE   ESC BACK",
        .hint_empty="ESC BACK",
        .empty="NOTHING TO CHOOSE HERE",
        .fill=fill, .user=fresh, .deadline_us=deadline,
    };
    pickmodal_open(&fresh->modal,&cfg);

    JSValue promise=pocket_api_promise_arm(ctx,request,&pick_ops,NULL,cancel,
                                           deadline);
    if(JS_IsException(promise)) {
        picker_close();
        pick_request=0;
        return promise;
    }
    ESP_LOGI(TAG,"PICK %u rows",fresh->modal.count);
    return promise;
}

void file_picker_reset(void) {
    // A screen still up when the session ends: the display goes back and the
    // completion follows, which is what pocket_api_reset() waits for.
    picker_finish(PICK_CANCELLED,false);
    pick_request=0;
    pick_chose=false;
}
