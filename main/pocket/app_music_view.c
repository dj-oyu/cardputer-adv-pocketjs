#include "app_music_view.h"
#include "app_legacy_presenter.h"
#include "pocket_api.h"
#include "pocket_av.h"
#include "ui/kasane/ksn_p0_probe.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MUSIC_INPUT_UNITS 256u
typedef struct {
    uint64_t key[12];
    ksn_presenter_values owned;
    ksn_ref refs[KSN_PRESENTER_ITEMS];
    uint32_t topology[KSN_PRESENTER_ITEMS];
    char system_status[KSN_PRESENTER_TEXT_MAX+1u];
    ksn_tx ticket;
    ksn_rgba topology_background;
    uint64_t system_until_us,title_revision,message_revision;
    uint64_t system_revision,manual_revision;
    uint32_t handle;
    int32_t playback_id;
    uint8_t system_bytes,topology_count;
    bool have_key,have_refs,manual,reactive,help;
} music_state;
_Static_assert(sizeof(music_state)<=1280,"Music view allocation budget");

static music_state *active;
static pocket_app_view_host current_host;
static uint32_t music_serial;
static JSClassID music_class;
static JSRuntime *music_rt;
static const JSClassDef music_def={.class_name="Kasane"};

static ksn_view *music_view(void){
    return current_host.view;
}
static ksn_rect music_viewport(void){
    return current_host.viewport;
}
static uint64_t music_now_us(void){
    return *current_host.now_us;
}
static bool music_busy(void){
    return current_host.busy(current_host.owner);
}
static void music_submitted(ksn_tx ticket,ksn_update_mode mode){
    current_host.submitted(current_host.owner,ticket,mode);
}

static const char *music_result_code(ksn_result result) {
    switch(result) {
    case KSN_OK: return "OK";
    case KSN_INVALID: return POCKET_ERR_INVALID_ARGUMENT;
    case KSN_LIMIT: return POCKET_ERR_LIMIT_EXCEEDED;
    case KSN_OOM: return POCKET_ERR_OUT_OF_MEMORY;
    case KSN_STALE: return POCKET_ERR_CLOSED;
    case KSN_BUSY: return POCKET_ERR_BUSY;
    case KSN_UNSUPPORTED: return POCKET_ERR_UNSUPPORTED;
    case KSN_IO: return POCKET_ERR_IO_ERROR;
    case KSN_CANCELLED: return POCKET_ERR_CANCELLED;
    }
    return POCKET_ERR_CONFLICT;
}

static JSValue music_throw_result(JSContext *ctx, ksn_result result, const char *op) {
    const char *code=music_result_code(result);
    return pocket_api_throw(ctx,code,op,code,
                            result==KSN_BUSY||result==KSN_OOM||result==KSN_IO,
                            POCKET_OUTCOME_NOT_APPLIED);
}

static bool music_number_in(JSContext *ctx, JSValueConst value, double lo, double hi,
                      double *out) {
    double n;
    if(!JS_IsNumber(value)||JS_ToFloat64(ctx,&n,value)<0||!isfinite(n)||
       floor(n)!=n||n<lo||n>hi) return false;
    *out=n;
    return true;
}


static bool presenter_text_equal(const char *a,uint8_t a_bytes,
                                 const char *b,uint8_t b_bytes){
    return a_bytes==b_bytes&&memcmp(a,b,a_bytes)==0;
}
static bool presenter_values_equal(const ksn_presenter_values *a,
                                   const ksn_presenter_values *b){
    for(unsigned i=0;i<KSN_PRESENTER_SLOTS;i++)
        if(!presenter_text_equal(a->text[i],a->bytes[i],b->text[i],b->bytes[i]))return false;
    return a->position_ms==b->position_ms&&a->duration_ms==b->duration_ms&&
           a->phase==b->phase&&a->playing==b->playing&&a->help==b->help;
}
static uint64_t presenter_viewport_key(void){
    return (uint16_t)music_viewport().x0|((uint64_t)(uint16_t)music_viewport().y0<<16)|
           ((uint64_t)(uint16_t)music_viewport().x1<<32)|((uint64_t)(uint16_t)music_viewport().y1<<48);
}
static uint64_t presenter_light_key(uint32_t phase,int track){
    int head=(int)(((uint64_t)phase*3u)%((unsigned)track+54u))-54;
    uint64_t key=0;
    for(unsigned i=0;i<3;i++){
        int x=head+(int)i*18,width=18;
        if(x<0){width+=x;x=0;}
        if(x+width>track)width=track-x;
        if(width<=0){x=0;width=0;}
        key|=((uint64_t)(((unsigned)x<<8)|(unsigned)width))<<(i*16);
    }
    return key;
}
static uint32_t presenter_shape(const ksn_presenter_item *item){
    return item->kind|((uint32_t)item->text_id<<8)|
           ((uint32_t)item->font<<16)|((uint32_t)item->radius<<24);
}
static bool presenter_can_patch(const music_state *p,const ksn_presenter_plan *plan){
    if(!plan->patchable||!p->have_refs||p->topology_count!=plan->count||
       p->topology_background!=plan->background)return false;
    for(unsigned i=0;i<plan->count;i++)
        if(p->topology[i]!=presenter_shape(&plan->items[i]))return false;
    return true;
}

/* Borrow source facts only within this owner turn. Compare the visible
 * projection before constructing a plan. The command bank owns submitted text;
 * pending work never points into a mutable source. */
static ksn_result music_refresh(void) {
    if(!active)return KSN_OK;
    music_state *p=active;
    if((!p->reactive&&!p->manual)||p->ticket.value)return KSN_OK;
    int width=music_viewport().x1-music_viewport().x0,height=music_viewport().y1-music_viewport().y0;
    if((width<25||height<28))return KSN_INVALID;
    uint64_t key[12]={0};
    key[0]=((uint64_t)p->manual<<8)|((uint64_t)p->reactive<<9);
    key[1]=presenter_viewport_key();
    pocket_av_ui_snapshot audio={0};
    bool live=false,system_status=false;
    if(p->manual)key[2]=p->manual_revision;
    else{
        key[2]=p->help?1u:0u;
        if(!p->help){
            live=pocket_av_ui_read(p->playback_id,&audio);
            if(live)ksn_p0_probe_copy(KSN_P0_PRODUCER_MATERIALIZED,sizeof(audio));
            system_status=p->system_bytes&&music_now_us()<p->system_until_us;
            key[3]=p->title_revision;
            if(system_status){key[4]=1;key[5]=p->system_revision;}
            else if(p->owned.bytes[1]){key[4]=2;key[5]=p->message_revision;}
            else if(live){
                key[4]=3;key[5]=(uint64_t)audio.state;
                key[6]=audio.position_ms/1000u;
                key[7]=audio.duration_ms/1000u;
                key[11]=audio.duration_ms?1u:0u;
            }
            if(live&&audio.duration_ms){
                uint64_t fill=(uint64_t)audio.position_ms*(unsigned)(width-24)/audio.duration_ms;
                key[10]=fill>(uint64_t)(width-24)?(uint64_t)(width-24):fill;
                if(key[10])key[9]=1;
            }else if(live&&audio.state==POCKET_AV_UI_PLAYING){
                key[10]=presenter_light_key((uint32_t)(music_now_us()/66667u),width-24);
                if(key[10])key[9]=2;
            }
        }
    }
    if(p->have_key&&memcmp(key,p->key,sizeof(key))==0)return KSN_OK;
    ksn_presenter_values values=p->owned;
    ksn_p0_probe_copy(KSN_P0_MUSIC_MODEL_COPY,sizeof(values));
    if(p->reactive){
        if(system_status){
            memcpy(values.text[1],p->system_status,p->system_bytes+1u);
            ksn_p0_probe_copy(KSN_P0_MUSIC_STATUS,p->system_bytes+1u);
            values.bytes[1]=p->system_bytes;
        }
        if(live){
            values.position_ms=audio.position_ms;
            values.duration_ms=audio.duration_ms;
            values.playing=audio.state==POCKET_AV_UI_PLAYING;
            if(!values.bytes[1]){
                static const char *const names[]={"READY","PLAYING","PAUSED","ENDED","ERROR"};
                if((unsigned)audio.state>=sizeof(names)/sizeof(names[0]))return KSN_INVALID;
                char line[KSN_PRESENTER_TEXT_MAX+1u];
                int n=snprintf(line,sizeof(line),"%s  %us",names[audio.state],
                               (unsigned)(audio.position_ms/1000u));
                if(audio.duration_ms&&n>0&&(size_t)n<sizeof(line))
                    n+=snprintf(line+n,sizeof(line)-(size_t)n," / %us",
                                (unsigned)(audio.duration_ms/1000u));
                ksn_p0_probe_copy(KSN_P0_MUSIC_MATERIALIZED,strlen(line)+1u);
                ksn_result r=ksn_presenter_copy_text(values.text[1],&values.bytes[1],
                                                       line,strlen(line));
                if(r!=KSN_OK)return r;
            }
        }
        values.phase=(uint32_t)(music_now_us()/66667u);
        values.help=p->help;
    }
    ksn_presenter_plan plan;
    ksn_result r=ksn_presenter_make(KSN_PRESENTER_MUSIC,&values,
        (uint16_t)(music_viewport().x1-music_viewport().x0),(uint16_t)(music_viewport().y1-music_viewport().y0),&plan);
    if(r!=KSN_OK)return r;
    if(music_busy())return KSN_BUSY;
    bool patch=presenter_can_patch(p,&plan);
    ksn_tx ticket;
    ksn_ref candidate_refs[KSN_PRESENTER_ITEMS];
    if(patch)r=ksn_presenter_patch(music_view(),music_viewport(),&plan,p->refs,&ticket);
    else r=ksn_presenter_submit(music_view(),music_viewport(),(ksn_resource){0},&plan,candidate_refs,&ticket);
    if(r!=KSN_OK)return r;
    if(!patch)memcpy(p->refs,candidate_refs,sizeof(candidate_refs));
    p->topology_count=plan.count;p->topology_background=plan.background;
    for(unsigned i=0;i<plan.count;i++)p->topology[i]=presenter_shape(&plan.items[i]);
    p->have_refs=false;
    memcpy(p->key,key,sizeof(key));p->have_key=true;p->ticket=ticket;
    music_submitted(ticket,patch?KSN_PATCH:KSN_REPLACE);
    return KSN_OK;
}


static ksn_result music_host_status(void *instance,const char *text,size_t bytes,
                                    uint64_t until_us){
    if(!instance||instance!=active)return KSN_OK;
    music_state *p=instance;
    if(p->manual)return KSN_UNSUPPORTED;
    char candidate[KSN_PRESENTER_TEXT_MAX+1u]={0};
    uint8_t count=0;
    ksn_result result=ksn_presenter_copy_text(candidate,&count,text,bytes);
    if(result!=KSN_OK)return result;
    bool changed=!presenter_text_equal(p->system_status,p->system_bytes,candidate,count);
    if(changed&&p->system_revision==UINT64_MAX)return KSN_LIMIT;
    memcpy(p->system_status,candidate,count+1u);
    ksn_p0_probe_copy(KSN_P0_MUSIC_STATUS,count+1u);
    if(changed)p->system_revision++;
    p->system_bytes=count;p->system_until_us=until_us;
    p->reactive=true;
    return music_refresh();
}

static ksn_result music_step(void *instance,bool *blocked){
    if(blocked)*blocked=false;
    if(!instance||instance!=active)return KSN_OK;
    music_state *p=instance;
    if(p->ticket.value){
        ksn_submission outcome=ksn_view_poll(music_view());
        if(outcome.ticket.value!=p->ticket.value)return KSN_STALE;
        if(outcome.status==KSN_SUBMITTED){if(blocked)*blocked=true;return KSN_OK;}
        if(outcome.status==KSN_DISCARDED){p->have_key=false;p->have_refs=false;}
        else if(outcome.status!=KSN_PRESENTED)return KSN_STALE;
        else p->have_refs=true;
        p->ticket=(ksn_tx){0};
    }
    ksn_result result=music_refresh();
    if(result==KSN_BUSY)result=KSN_OK;
    if(blocked)*blocked=p->ticket.value!=0;
    return result;
}

static bool presenter_text(JSContext *ctx,JSValueConst model,const char *name,
                           char out[KSN_PRESENTER_TEXT_MAX+1u],uint8_t *bytes){
    JSValue value=JS_GetPropertyStr(ctx,model,name);
    if(JS_IsException(value))return false;
    if(!JS_IsString(value)){
        JS_FreeValue(ctx,value);
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.view.update",
                         "presenter text must be a string",false,NULL);
        return false;
    }
    int64_t units;
    if(JS_GetLength(ctx,value,&units)<0){JS_FreeValue(ctx,value);return false;}
    if(units>MUSIC_INPUT_UNITS){
        JS_FreeValue(ctx,value);
        music_throw_result(ctx,KSN_LIMIT,"kasane.view.update");return false;
    }
    size_t length;const char *s=JS_ToCStringLen(ctx,&length,value);
    JS_FreeValue(ctx,value);
    if(!s)return false;
    ksn_p0_probe_copy(KSN_P0_UTF8_MATERIALIZED,length);
    ksn_result result=ksn_presenter_copy_text(out,bytes,s,length);
    JS_FreeCString(ctx,s);
    if(result==KSN_OK)return true;
    music_throw_result(ctx,result,"kasane.view.update");return false;
}
static bool presenter_number(JSContext *ctx,JSValueConst model,const char *name,
                             bool nullable,uint32_t *out){
    JSValue value=JS_GetPropertyStr(ctx,model,name);
    if(JS_IsException(value))return false;
    if(nullable&&(JS_IsNull(value)||JS_IsUndefined(value))){*out=0;JS_FreeValue(ctx,value);return true;}
    double n;bool ok=music_number_in(ctx,value,0,UINT32_MAX,&n);
    JS_FreeValue(ctx,value);
    if(ok){*out=(uint32_t)n;return true;}
    pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.view.update",
                     "presenter number must be a nonnegative integer",false,NULL);
    return false;
}
static bool presenter_bool(JSContext *ctx,JSValueConst model,const char *name,bool *out){
    JSValue value=JS_GetPropertyStr(ctx,model,name);
    if(JS_IsException(value))return false;
    if(!JS_IsBool(value)){
        JS_FreeValue(ctx,value);
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.view.update",
                         "presenter flag must be boolean",false,NULL);
        return false;
    }
    *out=JS_ToBool(ctx,value)!=0;JS_FreeValue(ctx,value);return true;
}
static JSValue js_presenter_update(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    uint32_t handle=(uint32_t)(uintptr_t)JS_GetOpaque(self,music_class);
    if(!handle||!active||active->handle!=handle)
        return music_throw_result(ctx,KSN_STALE,"kasane.view.update");
    if(active->reactive)return music_throw_result(ctx,KSN_BUSY,"kasane.view.update");
    JSValueConst model=argc?argv[0]:JS_UNDEFINED;
    if(!JS_IsObject(model))return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                         "kasane.view.update","model must be an object",false,NULL);
    ksn_presenter_values values={0};
    ksn_presenter_kind kind=KSN_PRESENTER_MUSIC;
    if(kind==KSN_PRESENTER_MUSIC){
        if(!presenter_text(ctx,model,"title",values.text[0],&values.bytes[0])||
           !presenter_text(ctx,model,"status",values.text[1],&values.bytes[1])||
           !presenter_number(ctx,model,"positionMs",false,&values.position_ms)||
           !presenter_number(ctx,model,"durationMs",true,&values.duration_ms)||
           !presenter_number(ctx,model,"phase",false,&values.phase)||
           !presenter_bool(ctx,model,"playing",&values.playing)||
           !presenter_bool(ctx,model,"help",&values.help))return JS_EXCEPTION;
    }else{
        return music_throw_result(ctx,KSN_UNSUPPORTED,"kasane.view.update");
    }
    uint16_t width=(uint16_t)(music_viewport().x1-music_viewport().x0);
    uint16_t height=(uint16_t)(music_viewport().y1-music_viewport().y0);
    ksn_presenter_plan plan;
    ksn_result result=ksn_presenter_make(kind,&values,width,height,&plan);
    if(result!=KSN_OK)return music_throw_result(ctx,result,"kasane.view.update");
    bool changed=!active->manual||
                 !presenter_values_equal(&active->owned,&values);
    if(changed&&active->manual_revision==UINT64_MAX)
        return music_throw_result(ctx,KSN_LIMIT,"kasane.view.update");
    active->owned=values;active->manual=true;
    ksn_p0_probe_copy(KSN_P0_MUSIC_MODEL_COPY,sizeof(values));
    if(changed)active->manual_revision++;
    result=music_refresh();
    return result==KSN_OK||result==KSN_BUSY?JS_UNDEFINED:
           music_throw_result(ctx,result,"kasane.view.update");
}
static music_state *presenter_owner(JSValueConst self){
    uint32_t handle=(uint32_t)(uintptr_t)JS_GetOpaque(self,music_class);
    return handle&&active&&active->handle==handle?
           active:NULL;
}
static bool presenter_optional_text(JSContext *ctx,JSValueConst model,const char *key,
                                    char out[KSN_PRESENTER_TEXT_MAX+1u],uint8_t *bytes){
    JSValue value=JS_GetPropertyStr(ctx,model,key);
    if(JS_IsException(value))return false;
    if(JS_IsUndefined(value)){JS_FreeValue(ctx,value);return true;}
    if(!JS_IsString(value)){
        JS_FreeValue(ctx,value);
        pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.view.set",
                         "slot value must be a string",false,NULL);
        return false;
    }
    int64_t units;
    if(JS_GetLength(ctx,value,&units)<0){JS_FreeValue(ctx,value);return false;}
    if(units>MUSIC_INPUT_UNITS){
        JS_FreeValue(ctx,value);
        music_throw_result(ctx,KSN_LIMIT,"kasane.view.set");return false;
    }
    size_t length;const char *text=JS_ToCStringLen(ctx,&length,value);
    JS_FreeValue(ctx,value);
    if(!text)return false;
    ksn_p0_probe_copy(KSN_P0_UTF8_MATERIALIZED,length);
    ksn_result result=ksn_presenter_copy_text(out,bytes,text,length);
    JS_FreeCString(ctx,text);
    if(result==KSN_OK)return true;
    music_throw_result(ctx,result,"kasane.view.set");return false;
}

static JSValue js_presenter_set(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    music_state *p=presenter_owner(self);
    if(!p)return music_throw_result(ctx,KSN_STALE,"kasane.view.set");
    if(p->manual)return music_throw_result(ctx,KSN_BUSY,"kasane.view.set");
        JSValueConst model=argc?argv[0]:JS_UNDEFINED;
    if(!JS_IsObject(model))return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,
                         "kasane.view.set","slots must be an object",false,NULL);
    ksn_presenter_values candidate=p->owned;
    ksn_p0_probe_copy(KSN_P0_MUSIC_MODEL_COPY,sizeof(candidate));
    if(!presenter_optional_text(ctx,model,"title",candidate.text[0],&candidate.bytes[0])||
       !presenter_optional_text(ctx,model,"message",candidate.text[1],&candidate.bytes[1]))
        return JS_EXCEPTION;
    bool title_changed=!presenter_text_equal(p->owned.text[0],p->owned.bytes[0],
                                             candidate.text[0],candidate.bytes[0]);
    bool message_changed=!presenter_text_equal(p->owned.text[1],p->owned.bytes[1],
                                               candidate.text[1],candidate.bytes[1]);
    if((title_changed&&p->title_revision==UINT64_MAX)||
       (message_changed&&p->message_revision==UINT64_MAX))
        return music_throw_result(ctx,KSN_LIMIT,"kasane.view.set");
    p->owned=candidate;
    ksn_p0_probe_copy(KSN_P0_MUSIC_MODEL_COPY,sizeof(candidate));
    p->reactive=true;
    if(title_changed)p->title_revision++;
    if(message_changed)p->message_revision++;
    ksn_result result=music_refresh();
    return result==KSN_OK||result==KSN_BUSY?JS_UNDEFINED:
           music_throw_result(ctx,result,"kasane.view.set");
}
static JSValue js_presenter_bind(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    music_state *p=presenter_owner(self);
    if(!p)return music_throw_result(ctx,KSN_STALE,"kasane.view.bind");
    if(p->manual)return music_throw_result(ctx,KSN_BUSY,"kasane.view.bind");
    const char *source=argc&&JS_IsString(argv[0])?JS_ToCString(ctx,argv[0]):NULL;
    if(!source)return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"kasane.view.bind",
                                     "source must be playback",false,NULL);
    bool playback=strcmp(source,"playback")==0;
    JS_FreeCString(ctx,source);
    if(!playback)
        return music_throw_result(ctx,KSN_UNSUPPORTED,"kasane.view.bind");
    int32_t id=pocket_av_ui_current_player();
    if(!id)return pocket_api_throw(ctx,POCKET_ERR_NOT_AVAILABLE,"kasane.view.bind",
                                  "no open player",false,NULL);
    p->playback_id=id;
    p->reactive=true;
    ksn_result result=music_refresh();
    return result==KSN_OK||result==KSN_BUSY?JS_UNDEFINED:
           music_throw_result(ctx,result,"kasane.view.bind");
}
static JSValue js_presenter_toggle_help(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argc;(void)argv;
    music_state *p=presenter_owner(self);
    if(!p)return music_throw_result(ctx,KSN_STALE,"kasane.view.toggleHelp");
    if(p->manual)return music_throw_result(ctx,KSN_BUSY,"kasane.view.toggleHelp");
        p->help=!p->help;p->reactive=true;
    ksn_result result=music_refresh();
    return result==KSN_OK||result==KSN_BUSY?JS_NewBool(ctx,p->help):
           music_throw_result(ctx,result,"kasane.view.toggleHelp");
}
static JSValue js_presenter_dismiss_help(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)argc;(void)argv;
    music_state *p=presenter_owner(self);
    if(!p)return music_throw_result(ctx,KSN_STALE,"kasane.view.dismissHelp");
    if(p->manual)return music_throw_result(ctx,KSN_BUSY,"kasane.view.dismissHelp");
        if(!p->help)return JS_FALSE;
    p->help=false;p->reactive=true;
    ksn_result result=music_refresh();
    return result==KSN_OK||result==KSN_BUSY?JS_TRUE:
           music_throw_result(ctx,result,"kasane.view.dismissHelp");
}
static const JSCFunctionListEntry presenter_methods[]={
    JS_CFUNC_DEF("update",1,js_presenter_update),
    JS_CFUNC_DEF("set",1,js_presenter_set),
    JS_CFUNC_DEF("bind",1,js_presenter_bind),
    JS_CFUNC_DEF("toggleHelp",0,js_presenter_toggle_help),
    JS_CFUNC_DEF("dismissHelp",0,js_presenter_dismiss_help),
};

static bool music_class_ready(JSContext *ctx){
    JSRuntime *rt=JS_GetRuntime(ctx);
    if(!pocket_api_class_ready(rt,&music_rt,&music_class)){
        JS_NewClassID(rt,&music_class);
        if(JS_NewClass(rt,music_class,&music_def)<0)return false;
    }
    JSValue proto=JS_GetClassProto(ctx,music_class);
    bool ready=!JS_IsNull(proto);
    JS_FreeValue(ctx,proto);
    if(ready)return true;
    proto=JS_NewObject(ctx);
    if(JS_IsException(proto))return false;
    if(JS_SetPropertyFunctionList(ctx,proto,presenter_methods,
       (int)(sizeof(presenter_methods)/sizeof(presenter_methods[0])))<0||
       JS_HasException(ctx)){
        JS_FreeValue(ctx,proto);return false;
    }
    JS_SetClassProto(ctx,music_class,proto);
    return true;
}

static JSValue music_mount(JSContext *ctx,const pocket_app_view_host *host,void **out){
    if(!host||!out||!host->view||!host->now_us||
       !host->busy||!host->submitted||active)
        return music_throw_result(ctx,KSN_BUSY,"kasane.mount");
    if(music_serial==UINT32_MAX)return music_throw_result(ctx,KSN_LIMIT,"kasane.mount");
    if(!music_class_ready(ctx))return JS_EXCEPTION;
    JSValue object=JS_NewObjectClass(ctx,music_class);
    if(JS_IsException(object))return object;
    music_state *p=calloc(1,sizeof(*p));
    if(!p){JS_FreeValue(ctx,object);return music_throw_result(ctx,KSN_OOM,"kasane.mount");}
    p->handle=++music_serial;
    JS_SetOpaque(object,(void *)(uintptr_t)p->handle);
    current_host=*host;active=p;*out=p;
    return object;
}

static size_t music_native_bytes(const void *instance){
    return instance?sizeof(music_state):0u;
}
static void music_destroy(void *instance){
    if(instance==active){active=NULL;current_host=(pocket_app_view_host){0};}
    free(instance);
}
const pocket_app_view_provider pocket_app_music_view={
    .name="music",.mount=music_mount,.step=music_step,
    .host_status=music_host_status,.native_bytes=music_native_bytes,
    .destroy=music_destroy
};
