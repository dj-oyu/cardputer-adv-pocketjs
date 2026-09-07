#include "pet_assets.h"
#include "esp_timer.h"
#include "pocket_api.h"
#include "app_session.h"
#include "pet_pixels.h"
#include <stdlib.h>
#include <string.h>
#include "paint.h"

extern const uint8_t pet_compact_start[] asm("_binary_pets_compact_bin_start");
extern const uint8_t pet_compact_end[] asm("_binary_pets_compact_bin_end");
static pocketjs_ui_core_t *pet_core;
static int32_t texture = -1;
static int32_t image_node = -1;
static int sprite=-1, sprite_x, sprite_y;
static int emotion;
static char speech[PET_SPEECH_CHARS+1];
static int64_t speech_at;
static int revealed;

static JSValue place(JSContext *ctx, JSValueConst self, int argc, JSValueConst *argv) {
    (void)self;
    int32_t id,x,y,mood=0;
    if(argc<3) return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"pet.place",
                                       "pet index, x, y required",false,NULL);
    if(JS_ToInt32(ctx,&id,argv[0]) || JS_ToInt32(ctx,&x,argv[1]) ||
       JS_ToInt32(ctx,&y,argv[2]) || (argc>3 && JS_ToInt32(ctx,&mood,argv[3]))) return JS_EXCEPTION;
    if(id < -1 || id>=12 || x < -64 || x>240 || y < -64 || y>135 || mood<0 || mood>5)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"pet.place",
                                "invalid pet placement",false,NULL);
    if(texture>=0) {
        pocketjs_ui_core_set_image(pet_core,image_node,-1);
        pocketjs_ui_core_free_texture(pet_core,texture);
        texture=-1;image_node=-1;
    }
    if(id!=sprite || x!=sprite_x || y!=sprite_y || mood!=emotion) {
        sprite=id; sprite_x=x; sprite_y=y; emotion=mood; app_force_redraw();
    }
    return JS_UNDEFINED;
}

static JSValue say(JSContext *ctx,JSValueConst self,int argc,JSValueConst *argv) {
    (void)self;
    if(!argc) return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"pet.say",
                                      "speech required",false,NULL);
    size_t n=0;const char *s=JS_ToCStringLen(ctx,&n,argv[0]);
    if(!s) return JS_EXCEPTION;
    // Too long and not printable are different answers: an app that read
    // maxSpeechChars can shorten the string, but it cannot make it ASCII.
    const char *code=n>PET_SPEECH_CHARS?POCKET_ERR_LIMIT_EXCEEDED:NULL;
    for(size_t i=0;i<n&&!code;i++)
        if((unsigned char)s[i]<32 || (unsigned char)s[i]>126) code=POCKET_ERR_INVALID_ARGUMENT;
    if(!code) { memcpy(speech,s,n+1); speech_at=esp_timer_get_time()/1000; revealed=0; app_force_redraw(); }
    JS_FreeCString(ctx,s);
    return code?pocket_api_throw(ctx,code,"pet.say","speech: up to 22 printable ASCII characters",
                                 false,NULL):JS_UNDEFINED;
}

void pet_assets_tick(void) {
    if(!speech[0]) return;
    int64_t age=esp_timer_get_time()/1000-speech_at;
    int count=(int)(age/70);int length=(int)strlen(speech);
    if(count>length) count=length;
    if(age>=2400) { speech[0]=0; app_force_redraw(); }
    else if(count!=revealed) { revealed=count; app_force_redraw(); }
}

void pet_assets_overlay(uint16_t *pixels,int y,int rows) {
    if(sprite>=0) pet_pixels_draw(pet_compact_start,(unsigned)sprite,pixels,240,y,rows,sprite_x,sprite_y,1,(unsigned)emotion);
    if(sprite>=0 && speech[0]) {
        paint_begin(pixels,y,rows);
        paint_fill(96,24,140,21,0x0864);paint_fill(97,25,138,19,0xff79);
        paint_fill(92,38,5,3,0xff79);
        char tail=speech[revealed];speech[revealed]=0;
        paint_ascii(100,31,speech,0x0864);speech[revealed]=tail;
    }
}

static JSValue image(JSContext *ctx, JSValueConst self, int argc,
                     JSValueConst *argv) {
    (void)self;
    int32_t node, index;
    if(argc < 2) return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"pet.show",
                                         "node and pet index required",false,NULL);
    if(JS_ToInt32(ctx, &node, argv[0]) ||
       JS_ToInt32(ctx, &index, argv[1])) return JS_EXCEPTION;
    // Compatibility path only, reached through __petImage; built-in apps use
    // place() with no texture. It keeps the "pet.show" operation name.
    if(!pet_core) return pocket_api_throw(ctx,POCKET_ERR_NOT_AVAILABLE,"pet.show",
                                          "no UI core",false,NULL);
    if(node <= 1 || index < 0 || index >= 12)
        return pocket_api_throw(ctx,POCKET_ERR_INVALID_ARGUMENT,"pet.show",
                                "invalid pet image",false,NULL);
    if(texture >= 0) {
        pocketjs_ui_core_set_image(pet_core,image_node,-1);
        pocketjs_ui_core_free_texture(pet_core, texture);
    }
    texture=-1;
    uint16_t *decoded=malloc(8192);
    // Nothing has been replaced yet -- place()'s sprite is still up, which is
    // why not-applied is honest here. A previous show() image is released
    // either way, and only a second show() can have left one.
    if(!decoded) return pocket_api_throw(ctx,POCKET_ERR_OUT_OF_MEMORY,"pet.show",
                                         "pet decode allocation failed",true,
                                         POCKET_OUTCOME_NOT_APPLIED);
    for(unsigned y=0;y<64;y++) {
        pet_pixels_row(pet_compact_start,(unsigned)index,y,decoded+y*64);
        pet_pixels_face((unsigned)index,y,0,decoded+y*64);
    }
    texture = pocketjs_ui_core_upload_texture(pet_core,(const uint8_t *)decoded,8192,64,64,2);
    free(decoded);
    // The Rust core aborts rather than reporting a failed allocation, so a
    // negative handle is some other refusal; core.h does not say which, and
    // this code is the best guess available.
    if(texture < 0) return pocket_api_throw(ctx,POCKET_ERR_LIMIT_EXCEEDED,"pet.show",
                                            "the UI core refused the texture",false,NULL);
    sprite=-1;speech[0]=0;
    pocketjs_ui_core_set_image(pet_core, node, texture);
    image_node=node;
    return JS_UNDEFINED;
}

static JSValue now(JSContext *ctx, JSValueConst self, int argc,
                   JSValueConst *argv) {
    (void)self; (void)argc; (void)argv;
    return JS_NewFloat64(ctx, esp_timer_get_time() / 1000.0);
}

// The second contributor to pocket.pet; pet_hub.c registered the first. Reading
// the namespace off the root to add to it would now build it, which is exactly
// what a contributor exists to avoid.
static esp_err_t build_pet_assets(JSContext *ctx, JSValueConst ns, void *user) {
    (void)user;
    JSValue pet=(JSValue)ns;
    JS_SetPropertyStr(ctx,pet,"show",JS_NewCFunction(ctx,image,"show",2));
    JS_SetPropertyStr(ctx,pet,"place",JS_NewCFunction(ctx,place,"place",3));
    JS_SetPropertyStr(ctx,pet,"say",JS_NewCFunction(ctx,say,"say",1));
    JS_SetPropertyStr(ctx,pet,"now",JS_NewCFunction(ctx,now,"now",0));
    return ESP_OK;
}

esp_err_t pet_assets_install(JSContext *ctx, void *core) {
    if(!pet_pixels_valid(pet_compact_start,(size_t)(pet_compact_end-pet_compact_start))) return ESP_ERR_INVALID_SIZE;
    pet_core = core;
    texture = -1;
    sprite=-1;
    speech[0]=0; emotion=0;
    // Eager, and not part of the namespace: these two are globals the pet app
    // calls directly, so nothing on the pocket root gates them.
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "__petImage", JS_NewCFunction(ctx, image, "__petImage", 2));
    JS_SetPropertyStr(ctx, global, "__petNow", JS_NewCFunction(ctx, now, "__petNow", 0));
    JS_FreeValue(ctx, global);
    return pocket_api_lazy(ctx,"pet",build_pet_assets,NULL);
}

void pet_assets_reset(void) {
    // The owning UI core frees its textures; never retain a dead core pointer.
    pet_core = NULL;
    texture = -1;
    image_node = -1;
    sprite=-1;
    speech[0]=0; emotion=0;
}
