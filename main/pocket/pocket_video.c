#include "pocket_video.h"
#include "pocket_api.h"
#include "pocket_av.h"
#include "pocket_kasane.h"
#include "pocket_video_sd_stream.h"
#include "ui/kasane/ksn_video_frames.h"
#if defined(KASANE_D5_SD_STREAM_PROBE) || defined(KASANE_D6_SD_AV_STREAM_PROBE)
#include "esp_log.h"
#endif
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ksn_video_frames frames;
    uint16_t *buffers[KSN_VIDEO_SLOTS];
    bool open;
    bool audio_seen;
    int32_t audio_player_id;
    uint32_t last_audio_ms;
    pocket_video_sd_stream *stream;
} pocket_video_state;
static pocket_video_state video;

static JSValue fail(JSContext *ctx,const char *op,const char *code,const char *message){
    return pocket_api_throw(ctx,code,op,message,false,POCKET_OUTCOME_NOT_APPLIED);
}
static bool integer(JSContext *ctx,JSValueConst value,double maximum,uint64_t *out){
    double number;
    if(!JS_IsNumber(value)||JS_ToFloat64(ctx,&number,value)<0||
       !isfinite(number)||number<0||number>maximum||floor(number)!=number)
        return false;
    *out=(uint64_t)number;return true;
}
static ksn_result read_span(void *ctx,uint16_t variant,uint16_t frame,
                            uint16_t y,uint16_t x,uint16_t count,
                            uint16_t *rgb565,uint8_t *alpha){
    pocket_video_state *state=ctx;
    if(!state||!state->open||variant||frame||y>=state->frames.height||
       x>state->frames.width||count>state->frames.width-x||
       (count&&(!rgb565||!alpha)))return KSN_INVALID;
    ksn_video_handle handle=state->frames.pending.generation?
        state->frames.pending:state->frames.committed;
    if(!handle.generation){
        if(count){memset(rgb565,0,(size_t)count*sizeof *rgb565);memset(alpha,255,count);}
        return KSN_OK;
    }
    ksn_image_port slot;
    ksn_video_image_port(&state->frames,&slot);
    return slot.read_span(slot.ctx,0,handle.slot,y,x,count,rgb565,alpha);
}
static void image_port(ksn_image_port *out){
    *out=(ksn_image_port){.ctx=&video,.width=video.frames.width,
        .height=video.frames.height,.variants=1,.frames=1,
        .read_span=read_span,.opaque=true};
}
void pocket_video_reset(void){
    if(video.stream){
        (void)pocket_video_sd_stream_stop(video.stream);
        video.stream=NULL;
    }
    for(unsigned i=0;i<KSN_VIDEO_SLOTS;i++)free(video.buffers[i]);
    memset(&video,0,sizeof video);
}
static JSValue stream_start(JSContext *ctx,JSValueConst self,int argc,
                            JSValueConst *argv){
    (void)self;
    const char *op="kasane.video.streamStart";
    if(!video.open||argc!=1||!JS_IsString(argv[0]))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,
                    "expected an open video and granted SD path");
    if(video.stream)return fail(ctx,op,POCKET_ERR_BUSY,"stream already active");
    const char *path=JS_ToCString(ctx,argv[0]);
    if(!path)return JS_EXCEPTION;
    const char *code=NULL;
    pocket_video_sd_stream *stream=pocket_video_sd_stream_start(path,
        video.frames.width,video.frames.height,&code);
    JS_FreeCString(ctx,path);
    if(!stream)return fail(ctx,op,code?code:POCKET_ERR_IO_ERROR,
                            "stream could not open");
    if(ksn_video_timeline_reset(&video.frames)!=KSN_VIDEO_OK){
        (void)pocket_video_sd_stream_stop(stream);
        return fail(ctx,op,POCKET_ERR_BUSY,
                    "pending video candidate or repair must finish first");
    }
    video.stream=stream;
    return JS_NewBool(ctx,true);
}
static JSValue stream_poll(JSContext *ctx,JSValueConst self,int argc,
                           JSValueConst *argv){
    (void)self;
    const char *op="kasane.video.streamPoll";
    uint64_t clock_us;
    if(!video.stream||argc!=1||
       !integer(ctx,argv[0],9007199254740991.0,&clock_us))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,
                    "expected an active stream and integer clock");
    pocket_video_sd_stream_clock(video.stream,clock_us);
    const uint8_t *pixels=NULL;uint32_t bytes=0;uint64_t pts=0;
    bool copied=false;
    if(pocket_video_sd_stream_borrow(video.stream,&pixels,&bytes,&pts)){
        ksn_video_handle handle;uint16_t *destination;
        if(ksn_video_begin(&video.frames,&handle,&destination)==KSN_VIDEO_OK){
            if(bytes==(uint32_t)video.frames.width*video.frames.height*2){
                /* ESP32-S3 is little endian, matching KSV1 RGB565 words. */
                memcpy(destination,pixels,bytes);
                copied=ksn_video_publish(&video.frames,handle,pts)==KSN_VIDEO_OK;
            }
            if(!copied)(void)ksn_video_abort_write(&video.frames,handle);
        }
        pocket_video_sd_stream_release(video.stream);
    }
    if(!pocket_kasane_video_can_select())return JS_NewBool(ctx,false);
    ksn_video_handle chosen;
    ksn_video_result result=ksn_video_select(&video.frames,clock_us,&chosen);
    if(result==KSN_VIDEO_OK){
#if defined(KASANE_D5_SD_STREAM_PROBE) || defined(KASANE_D6_SD_AV_STREAM_PROBE)
#ifdef KASANE_D5_SD_STREAM_PROBE
        const char *tag="D5_STREAM";
#else
        const char *tag="VIDEO_STREAM";
#endif
        ESP_LOGI(tag,"SELECT pts=%llu clock=%llu slot=%u",
                 (unsigned long long)video.frames.slots[chosen.slot].pts_us,
                 (unsigned long long)clock_us,(unsigned)chosen.slot);
#endif
        pocket_kasane_video_invalidate();return JS_NewBool(ctx,true);
    }
    if(result==KSN_VIDEO_EMPTY||result==KSN_VIDEO_BUSY)
        return JS_NewBool(ctx,false);
    return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"frame selection failed");
}
static JSValue stream_pause(JSContext *ctx,JSValueConst self,int argc,
                            JSValueConst *argv){
    (void)self;
    if(!video.stream||argc!=1||!JS_IsBool(argv[0]))
        return fail(ctx,"kasane.video.streamPause",POCKET_ERR_INVALID_ARGUMENT,
                    "expected active stream and boolean");
    pocket_video_sd_stream_pause(video.stream,JS_ToBool(ctx,argv[0]));
    return JS_UNDEFINED;
}
static JSValue stream_state(JSContext *ctx,JSValueConst self,int argc,
                            JSValueConst *argv){
    (void)self;(void)argv;
    if(argc!=0)return fail(ctx,"kasane.video.streamState",
                           POCKET_ERR_INVALID_ARGUMENT,"expected no arguments");
    int state=video.stream?pocket_video_sd_stream_state(video.stream):2;
    return JS_NewString(ctx,state==0?"reading":state==1?"end":
                            state==2?"closed":"error");
}
static JSValue stream_stop(JSContext *ctx,JSValueConst self,int argc,
                           JSValueConst *argv){
    (void)self;(void)argv;
    if(argc!=0)return fail(ctx,"kasane.video.streamStop",
                           POCKET_ERR_INVALID_ARGUMENT,"expected no arguments");
    bool stopped=pocket_video_sd_stream_stop(video.stream);
    video.stream=NULL;
    return JS_NewBool(ctx,stopped);
}
bool pocket_video_pending(void){return video.open&&video.frames.pending.generation;}
void pocket_video_present_result(ksn_result result){
    if(pocket_video_pending()&&result==KSN_OK)
        (void)ksn_video_finish(&video.frames,video.frames.pending,true);
    /* IO leaves Kasane's sealed submission intact; retain the selected slot. */
}
static JSValue open_video(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.video.open";
    uint64_t width,height;
    if(argc!=2||!integer(ctx,argv[0],240,&width)||
       !integer(ctx,argv[1],135,&height)||!width||!height||
       width*height>KSN_VIDEO_MAX_PIXELS)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected small RGB565 dimensions");
    bool created=!video.open;
    if(video.open){
        if(video.frames.width!=width||video.frames.height!=height)
            return fail(ctx,op,POCKET_ERR_BUSY,"video dimensions fixed for this session");
    }else{
        size_t bytes=(size_t)width*height*sizeof(uint16_t);
        for(unsigned i=0;i<KSN_VIDEO_SLOTS;i++){
            video.buffers[i]=malloc(bytes);
            if(!video.buffers[i]){
                pocket_video_reset();
                return fail(ctx,op,POCKET_ERR_OUT_OF_MEMORY,"frame pool allocation failed");
            }
        }
        if(!ksn_video_frames_init(&video.frames,(uint16_t)width,(uint16_t)height,
                                  video.buffers,(size_t)width*height)){
            pocket_video_reset();
            return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"invalid frame pool");
        }
        video.open=true;
    }
    ksn_image_port port;image_port(&port);
    JSValue resource=pocket_kasane_video_resource(ctx,&port);
    if(created&&JS_IsException(resource))pocket_video_reset();
    return resource;
}
static JSValue push_video(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.video.push";
    uint64_t pts;
    if(!video.open||argc!=2||!integer(ctx,argv[1],9007199254740991.0,&pts)||
       JS_GetTypedArrayType(argv[0])!=JS_TYPED_ARRAY_UINT16)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected Uint16Array and integer PTS in microseconds");
    size_t offset=0,bytes=0,element=0;
    JSValue buffer=JS_GetTypedArrayBuffer(ctx,argv[0],&offset,&bytes,&element);
    if(JS_IsException(buffer))return buffer;
    size_t size=0;
    uint8_t *raw=JS_GetArrayBuffer(ctx,&size,buffer);
    if(!raw||element!=2||bytes!=(size_t)video.frames.width*video.frames.height*2||
       offset>size||bytes>size-offset){
        JS_FreeValue(ctx,buffer);
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"pixel count does not match video dimensions");
    }
    ksn_video_handle handle;uint16_t *destination;
    ksn_video_result result=ksn_video_begin(&video.frames,&handle,&destination);
    if(result==KSN_VIDEO_BUSY){JS_FreeValue(ctx,buffer);return JS_NewBool(ctx,false);}
    if(result!=KSN_VIDEO_OK){JS_FreeValue(ctx,buffer);
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"frame pool unavailable");}
    memcpy(destination,raw+offset,bytes);
    JS_FreeValue(ctx,buffer);
    if(ksn_video_publish(&video.frames,handle,pts)!=KSN_VIDEO_OK){
        (void)ksn_video_abort_write(&video.frames,handle);
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"frame publication failed");
    }
    return JS_NewBool(ctx,true);
}
static JSValue select_video(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;
    const char *op="kasane.video.select";
    uint64_t clock_us;
    if(!video.open||argc!=1||
       !integer(ctx,argv[0],9007199254740991.0,&clock_us))
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected display clock in microseconds");
    if(!pocket_kasane_video_can_select())return JS_NewBool(ctx,false);
    ksn_video_handle handle;
    ksn_video_result result=ksn_video_select(&video.frames,clock_us,&handle);
    if(result==KSN_VIDEO_EMPTY||result==KSN_VIDEO_BUSY)return JS_NewBool(ctx,false);
    if(result!=KSN_VIDEO_OK)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"frame selection failed");
    pocket_kasane_video_invalidate();
    return JS_NewBool(ctx,true);
}
static JSValue select_audio(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argv;
    const char *op="kasane.video.selectAudio";
    if(!video.open||argc!=0)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected no arguments and an open video");
    int32_t id=pocket_av_ui_current_player();
    pocket_av_ui_snapshot clock;
    if(!id||!pocket_av_ui_read(id,&clock)||
       (clock.state!=POCKET_AV_UI_PLAYING&&clock.state!=POCKET_AV_UI_PAUSED))
        return JS_NewBool(ctx,false);
    if(!pocket_kasane_video_can_select())return JS_NewBool(ctx,false);
    if(video.audio_seen &&
       (id!=video.audio_player_id||clock.position_ms<video.last_audio_ms)){
        if(ksn_video_timeline_reset(&video.frames)!=KSN_VIDEO_OK)
            return JS_NewBool(ctx,false);
    }
    video.audio_seen=true;
    video.audio_player_id=id;
    video.last_audio_ms=clock.position_ms;
    ksn_video_handle handle;
    ksn_video_result result=ksn_video_select(&video.frames,
        (uint64_t)clock.position_ms*1000u,&handle);
    if(result==KSN_VIDEO_EMPTY||result==KSN_VIDEO_BUSY)return JS_NewBool(ctx,false);
    if(result!=KSN_VIDEO_OK)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"frame selection failed");
    pocket_kasane_video_invalidate();
    return JS_NewBool(ctx,true);
}
static JSValue reset_timeline(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;(void)argv;
    const char *op="kasane.video.resetTimeline";
    if(!video.open||argc!=0)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"expected no arguments and an open video");
    if(!pocket_kasane_video_can_select())return JS_NewBool(ctx,false);
    ksn_video_result result=ksn_video_timeline_reset(&video.frames);
    if(result==KSN_VIDEO_BUSY)return JS_NewBool(ctx,false);
    if(result!=KSN_VIDEO_OK)
        return fail(ctx,op,POCKET_ERR_INVALID_ARGUMENT,"frame pool unavailable");
    video.audio_seen=false;
    return JS_NewBool(ctx,true);
}
esp_err_t pocket_video_install(JSContext *ctx,JSValueConst kasane){
    JSValue object=JS_NewObject(ctx);
    if(JS_IsException(object))return ESP_ERR_NO_MEM;
    static const JSCFunctionListEntry functions[]={
        JS_CFUNC_DEF("open",2,open_video),
        JS_CFUNC_DEF("push",2,push_video),
        JS_CFUNC_DEF("select",1,select_video),
        JS_CFUNC_DEF("selectAudio",0,select_audio),
        JS_CFUNC_DEF("resetTimeline",0,reset_timeline),
        JS_CFUNC_DEF("streamStart",1,stream_start),
        JS_CFUNC_DEF("streamPoll",1,stream_poll),
        JS_CFUNC_DEF("streamPause",1,stream_pause),
        JS_CFUNC_DEF("streamState",0,stream_state),
        JS_CFUNC_DEF("streamStop",0,stream_stop)
    };
    if(JS_SetPropertyFunctionList(ctx,object,functions,
         sizeof functions/sizeof functions[0])<0){
        JS_FreeValue(ctx,object);return ESP_ERR_NO_MEM;
    }
    if(JS_SetPropertyStr(ctx,(JSValue)kasane,"video",object)<0)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}
