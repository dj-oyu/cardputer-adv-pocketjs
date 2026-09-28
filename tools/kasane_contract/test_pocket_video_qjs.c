#include "quickjs.h"
#include "pocket_video.h"
#include "pocket_api.h"
#include "pocket_av.h"
#include "pocket_video_sd_stream.h"
#include "core_fixture.h"
#include "ksn_render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"video JS test %s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while (0)

static ksn_image_port image;
static unsigned invalidates;
static bool selectable=true;
static bool resource_fail_once=true;
static pocket_av_ui_snapshot audio_clock;
static bool audio_open;
struct pocket_video_sd_stream { uint64_t clock; bool paused, ready; };
static struct pocket_video_sd_stream fake_stream;
static uint16_t stream_pixels[8];
pocket_video_sd_stream *pocket_video_sd_stream_start(const char *path,
    uint16_t width,uint16_t height,const char **code){
    (void)code;
    CHECK(!strcmp(path,"sd:/granted/video.ksv")&&width==4&&height==2);
    fake_stream=(struct pocket_video_sd_stream){.ready=true};
    for(unsigned i=0;i<8;i++)stream_pixels[i]=0xeeee;
    return &fake_stream;
}
void pocket_video_sd_stream_clock(pocket_video_sd_stream *s,uint64_t clock){s->clock=clock;}
void pocket_video_sd_stream_pause(pocket_video_sd_stream *s,bool paused){s->paused=paused;}
bool pocket_video_sd_stream_borrow(pocket_video_sd_stream *s,
    const uint8_t **pixels,uint32_t *bytes,uint64_t *pts){
    if(s->paused||!s->ready||s->clock<400)return false;
    *pixels=(const uint8_t *)stream_pixels;*bytes=sizeof stream_pixels;*pts=400;
    return true;
}
void pocket_video_sd_stream_release(pocket_video_sd_stream *s){s->ready=false;}
int pocket_video_sd_stream_state(const pocket_video_sd_stream *s){(void)s;return 0;}
bool pocket_video_sd_stream_stop(pocket_video_sd_stream *s){CHECK(s==&fake_stream);return true;}
KSN_TEST_CORE(core,static);
static ksn_resource image_resource;
static uint16_t panel[240*135],strip_pixels[240*8];
static unsigned sends,fail_on_send;
static uint16_t *display_buffer(void *ctx){(void)ctx;return strip_pixels;}
static ksn_result display_send(void *ctx,uint16_t y,uint16_t rows,
                               const uint16_t *pixels){
    (void)ctx;
    if(++sends==fail_on_send)return KSN_IO;
    memcpy(panel+(size_t)y*240,pixels,(size_t)rows*240*sizeof *pixels);
    return KSN_OK;
}
int32_t pocket_av_ui_current_player(void){return audio_open?42:0;}
bool pocket_av_ui_read(int32_t id,pocket_av_ui_snapshot *out){
    if(id!=42||!audio_open||!out)return false;
    *out=audio_clock;return true;
}
JSValue pocket_kasane_video_resource(JSContext *ctx,const ksn_image_port *port){
    if(resource_fail_once){
        resource_fail_once=false;
        CHECK(port->width==3&&port->height==2);
        return JS_ThrowTypeError(ctx,"injected registration failure");
    }
    CHECK(port->width==4&&port->height==2&&port->frames==1);
    image=*port;
    CHECK(ksn_core_register_image(&core,KSN_APP,port,&image_resource)==KSN_OK);
    return JS_NewObject(ctx);
}
bool pocket_kasane_video_can_select(void){return selectable;}
void pocket_kasane_video_invalidate(void){
    ++invalidates;
    bool drawn=ksn_core_invalidate_image(&core,image_resource);
    CHECK(drawn==(invalidates>1));
}
JSValue pocket_api_throw(JSContext *ctx,const char *code,const char *op,
                         const char *message,bool retryable,const char *outcome){
    (void)retryable;(void)outcome;
    return JS_ThrowTypeError(ctx,"%s %s: %s",code,op,message);
}
static void eval(JSContext *ctx,const char *script,bool exception){
    JSValue value=JS_Eval(ctx,script,strlen(script),"video.js",JS_EVAL_TYPE_GLOBAL);
    if(JS_IsException(value)&&!exception){
        JSValue error=JS_GetException(ctx);
        const char *message=JS_ToCString(ctx,error);
        fprintf(stderr,"unexpected JS exception: %s\n",message?message:"unknown");
        if(message)JS_FreeCString(ctx,message);
        JS_FreeValue(ctx,error);
    }
    CHECK(JS_IsException(value)==exception);
    if(exception){JSValue error=JS_GetException(ctx);JS_FreeValue(ctx,error);}
    JS_FreeValue(ctx,value);
}
static void expect_pixel(uint16_t value){
    uint16_t pixels[2];uint8_t alpha[2];
    CHECK(image.read_span(image.ctx,0,0,1,1,2,pixels,alpha)==KSN_OK);
    CHECK(pixels[0]==value&&pixels[1]==value&&alpha[0]==255&&alpha[1]==255);
}
int main(void){
    ksn_core_init(&core);
    JSRuntime *runtime=JS_NewRuntime();CHECK(runtime);
    JSContext *ctx=JS_NewContext(runtime);CHECK(ctx);
    JSValue kasane=JS_NewObject(ctx);CHECK(!JS_IsException(kasane));
    CHECK(pocket_video_install(ctx,kasane)==ESP_OK);
    JSValue global=JS_GetGlobalObject(ctx);
    CHECK(JS_SetPropertyStr(ctx,global,"kasane",kasane)>=0);
    JS_FreeValue(ctx,global);

    eval(ctx,"kasane.video.open(3,2)",true);

    eval(ctx,"globalThis.resource=kasane.video.open(4,2);"
             "globalThis.data=new Uint16Array(8).fill(0x1234);"
             "if(!kasane.video.push(data,100))throw Error('push');"
             "if(kasane.video.select(99))throw Error('early');"
             "if(!kasane.video.select(100))throw Error('select');",false);
    CHECK(invalidates==1&&pocket_video_pending());
    expect_pixel(0x1234);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_client system=ksn_core_client(&core,KSN_SYSTEM);
    ksn_tx tx;ksn_ref ref;
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={10,3,14,5},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=image_resource,.scale=KSN_IMAGE_1X}};
    ksn_draw notice={.kind=KSN_RECT,.bounds={16,3,20,5},
        .clip={0,0,240,135},.opacity=255,
        .data.shape={0x00ff00ff,0,0}};
    ksn_ref notice_ref;
    CHECK(system.ops->begin(system.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(system.ops->add(system.ctx,tx,&notice,&notice_ref)==KSN_OK);
    CHECK(system.ops->end(system.ctx,tx)==KSN_OK);
    ksn_display_port display={NULL,display_buffer,display_send,240,135,8,NULL,NULL};
    ksn_render_stats stats;
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    CHECK(app.ops->add(app.ctx,tx,&draw,&ref)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    sends=0;fail_on_send=2;
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_IO);
    CHECK(panel[3*240+10]==0x1234);
    CHECK(ksn_core_reset_layer(&core,KSN_APP)==KSN_BUSY);
    pocket_video_present_result(KSN_IO);
    CHECK(pocket_video_pending());expect_pixel(0x1234);
    eval(ctx,"if(kasane.video.select(200))throw Error('pending accepted');",false);
    sends=0;fail_on_send=0;
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(panel[3*240+10]==0x1234&&panel[3*240+16]==0x07e0);
    pocket_video_present_result(KSN_OK);
    CHECK(!pocket_video_pending());expect_pixel(0x1234);

    eval(ctx,"data.fill(0x4567);"
             "if(!kasane.video.push(data,200))throw Error('push2');"
             "if(kasane.video.select(199))throw Error('early2');",false);
    selectable=false;
    eval(ctx,"if(kasane.video.select(200))throw Error('busy');",false);
    selectable=true;
    eval(ctx,"if(!kasane.video.select(200))throw Error('select2');",false);
    expect_pixel(0x4567);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(panel[3*240+10]==0x4567&&panel[3*240+16]==0x07e0);
    pocket_video_present_result(KSN_OK);
    eval(ctx,"if(kasane.video.selectAudio())throw Error('no audio');",false);
    audio_open=true;
    audio_clock=(pocket_av_ui_snapshot){.state=POCKET_AV_UI_PLAYING,.position_ms=300};
    eval(ctx,"data.fill(0x7878);kasane.video.push(data,300000);"
             "if(!kasane.video.selectAudio())throw Error('audio select');",false);
    expect_pixel(0x7878);
    pocket_video_present_result(KSN_OK);
    audio_clock.position_ms=100;
    eval(ctx,"data.fill(0x9999);kasane.video.push(data,100000);"
             "if(kasane.video.selectAudio())throw Error('old epoch frame selected');"
             "kasane.video.push(data,100000);"
             "if(!kasane.video.selectAudio())throw Error('seek select');",false);
    expect_pixel(0x9999);
    pocket_video_present_result(KSN_OK);
    eval(ctx,"if(!kasane.video.resetTimeline())throw Error('reset');"
             "data.fill(0xabcd);kasane.video.push(data,0);"
             "if(!kasane.video.select(0))throw Error('manual restart');",false);
    expect_pixel(0xabcd);
    pocket_video_present_result(KSN_OK);
    eval(ctx,"kasane.video.push(new Uint16Array(7),300)",true);
    eval(ctx,"kasane.video.push(new Int16Array(8),300)",true);
    eval(ctx,"kasane.video.push(data,-1)",true);
    eval(ctx,"kasane.video.open(5,2)",true);
    eval(ctx,"if(kasane.video.streamState()!=='closed')throw Error('closed');"
             "kasane.video.streamStart('sd:/granted/video.ksv');"
             "kasane.video.streamPause(true);"
             "if(kasane.video.streamPoll(400))throw Error('paused');"
             "kasane.video.streamPause(false);"
             "if(kasane.video.streamPoll(399))throw Error('early stream');"
             "if(!kasane.video.streamPoll(400))throw Error('stream frame');",false);
    expect_pixel(0xeeee);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    pocket_video_present_result(KSN_OK);
    eval(ctx,"if(!kasane.video.streamStop())throw Error('stop');"
             "if(kasane.video.streamState()!=='closed')throw Error('closed2');",false);
    CHECK(invalidates==6);
    CHECK(ksn_core_reset_layer(&core,KSN_APP)==KSN_OK);
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(panel[3*240+10]==0&&panel[3*240+16]==0x07e0);
    pocket_video_reset();
    JS_FreeContext(ctx);JS_FreeRuntime(runtime);
    puts("pocket video QuickJS PASS");
    return 0;
}
