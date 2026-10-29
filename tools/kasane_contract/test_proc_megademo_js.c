#include "pocket_shape.h"
#include "shape_draw_shim.h"
#include "quickjs.h"
#include "ksn_proc_plan.h"
#include "proc_megademo.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ksn_proc_plan js_plans[3][PROC_MEGA_LAYERS];
static ksn_proc_plan c_plans[3][PROC_MEGA_LAYERS];
static uint16_t js_pixels[KSN_PROC_W * KSN_PROC_H];
static uint16_t c_pixels[KSN_PROC_W * KSN_PROC_H];
static ksn_proc_frame js_frame, c_frame;

#define REQUIRE(test) do { if (!(test)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #test); exit(1); \
} } while (0)

static JSValue call2(JSContext *ctx, JSValueConst object, const char *name,
                     int first, int second) {
    JSValue function = JS_GetPropertyStr(ctx, object, name);
    REQUIRE(!JS_IsException(function));
    JSValue args[2] = { JS_NewInt32(ctx, first), JS_NewInt32(ctx, second) };
    JSValue result = JS_Call(ctx, function, object, 2, args);
    JS_FreeValue(ctx, args[0]);
    JS_FreeValue(ctx, args[1]);
    JS_FreeValue(ctx, function);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        fprintf(stderr, "procMegademo.%s(%d,%d): %s\n", name, first, second,
                message ? message : "exception");
        if (message) JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
        exit(1);
    }
    return result;
}

static double number_at(JSContext *ctx, JSValueConst array, unsigned index) {
    JSValue value = JS_GetPropertyUint32(ctx, array, index);
    double number = 0;
    REQUIRE(!JS_IsException(value) && JS_ToFloat64(ctx, &number, value) == 0);
    JS_FreeValue(ctx, value);
    REQUIRE(isfinite(number));
    return number;
}

static unsigned length(JSContext *ctx, JSValueConst array) {
    JSValue value = JS_GetPropertyStr(ctx, array, "length");
    uint32_t count = 0;
    REQUIRE(!JS_IsException(value) && JS_ToUint32(ctx, &count, value) == 0);
    JS_FreeValue(ctx, value);
    return count;
}

static void read_program(JSContext *ctx, JSValueConst object, unsigned phase,
                         unsigned layer, ksn_proc_inst code[KSN_PROC_CODE],
                         ksn_proc_program *program) {
    JSValue array = call2(ctx, object, "program", (int)phase, (int)layer);
    unsigned count = length(ctx, array);
    REQUIRE(count > 0 && count <= KSN_PROC_CODE);
    for (unsigned i = 0; i < count; ++i) {
        JSValue entry = JS_GetPropertyUint32(ctx, array, i);
        REQUIRE(!JS_IsException(entry) && length(ctx, entry) == 6);
        double fields[6];
        for (unsigned j = 0; j < 6; ++j) fields[j] = number_at(ctx, entry, j);
        REQUIRE(fields[0] >= 0 && fields[0] <= KSN_PROC_LINE_COLOR_REG);
        REQUIRE(fields[1] >= 0 && fields[1] <= 255);
        REQUIRE(fields[2] >= 0 && fields[2] <= 255);
        REQUIRE(fields[3] >= 0 && fields[3] <= 255);
        REQUIRE(fields[5] >= 0 && fields[5] <= 65535);
        memset(&code[i], 0, sizeof code[i]);
        code[i].op = (uint8_t)fields[0]; code[i].dst = (uint8_t)fields[1];
        code[i].a = (uint8_t)fields[2]; code[i].b = (uint8_t)fields[3];
        code[i].value = (float)fields[4]; code[i].color = (uint16_t)fields[5];
        JS_FreeValue(ctx, entry);
    }
    JS_FreeValue(ctx, array);
    program->code = code;
    program->count = (uint8_t)count;
}

static void read_inputs(JSContext *ctx, JSValueConst object, unsigned frame,
                        unsigned layer, float input[KSN_PROC_INPUTS]) {
    JSValue array = call2(ctx, object, "inputs", (int)frame, (int)layer);
    /* The demo passes four inputs; draw() zero-pads to KSN_PROC_INPUTS. */
    const unsigned n = length(ctx, array);
    REQUIRE(n == 4 && n <= KSN_PROC_INPUTS);
    for (unsigned i = 0; i < KSN_PROC_INPUTS; ++i)
        input[i] = i < n ? (float)number_at(ctx, array, i) : 0.0f;
    JS_FreeValue(ctx, array);
}

static uint16_t read_backdrop(JSContext *ctx, JSValueConst object,
                              unsigned frame) {
    JSValue function = JS_GetPropertyStr(ctx, object, "backdrop");
    JSValue arg = JS_NewInt32(ctx, (int)frame);
    JSValue value = JS_Call(ctx, function, object, 1, &arg);
    uint32_t color = 0;
    REQUIRE(!JS_IsException(value) && JS_ToUint32(ctx, &color, value) == 0 &&
            color <= 65535);
    JS_FreeValue(ctx, value);
    JS_FreeValue(ctx, arg);
    JS_FreeValue(ctx, function);
    return (uint16_t)color;
}

static char *read_file(const char *path, size_t *size) {
    FILE *file = fopen(path, "rb");
    REQUIRE(file);
    REQUIRE(fseek(file, 0, SEEK_END) == 0);
    long n = ftell(file);
    REQUIRE(n > 0 && fseek(file, 0, SEEK_SET) == 0);
    char *source = malloc((size_t)n + 1);
    REQUIRE(source && fread(source, 1, (size_t)n, file) == (size_t)n);
    source[n] = 0;
    REQUIRE(fclose(file) == 0);
    *size = (size_t)n;
    return source;
}

int main(int argc, char **argv) {
    REQUIRE(argc == 2);
    JSRuntime *runtime = JS_NewRuntime(); REQUIRE(runtime);
    JSContext *ctx = JS_NewContext(runtime); REQUIRE(ctx);
    /* The view side is permissive (every tx method returns a ref); the
     * procedural side records registrations, live handles and frames, and
     * throws on a draw or unregister of a handle that is not live. The real
     * validators run in run_megademo_app_host.py. */
    static const char mock[] =
        "globalThis.__procCalls={registered:[],live:new Set(),peak:0,unregistered:0,"
        "frames:[],active:null,images:[],resize:[]};"
        "globalThis.console={log(){}};"
        "const ref=()=>({visible:true,setRect(){},setClip(){},setColor(){},setText(){},"
        "setReveal(){},setImageFrame(){},setRotation(){},place(){},"
        "animate(){return {stop(){},finish(){},poll(){return 'running'}}},"
        "setVisible(tx,v){this.visible=v}});"
        "const uiTx=new Proxy({},{get(o,k){return k==='image'?"
        "s=>{const i=ref();i.spec=s;__procCalls.images.push(i);return i}:()=>ref()}});"
        "globalThis.pocket={memory:{info(){return {internalFreeBytes:1e9}}},"
        "kasane:{replace(f){f(uiTx)},patch(f){f(uiTx)},"
        "stats(){return {displayed:{commands:0}}},resource(){return {}},"
        "cache:{create(){return {}}},pixel:{open(){return {}},stage(){return true}},"
        "grid:{registerResizeSource(s){__procCalls.resize.push(s);return 76+__procCalls.resize.length},"
        "resource(h){if(h!==77&&h!==78)throw Error('resize handle');return {resized:h}}},procedural:{"
        "resource(){return {}},createSurface(){return 9},"
        "register(p,b){const id=__procCalls.registered.length+1;"
        "__procCalls.registered.push({program:p,batch:b});__procCalls.live.add(id);"
        "if(__procCalls.live.size>__procCalls.peak)__procCalls.peak=__procCalls.live.size;return id},"
        "unregister(h){if(!__procCalls.live.delete(h))throw Error('stale '+h);__procCalls.unregistered++},"
        /* A beginFrame with nothing drawn yet restarts, as the native one
         * does: the app opens both surfaces once at start to allocate them. */
        "beginFrame(c,s){if(__procCalls.active!==null&&__procCalls.active.draws.length)"
        "throw Error('nested frame');"
        "__procCalls.active={backdrop:c,surface:s||0,draws:[]}},"
        "draw(h,...i){if(!__procCalls.live.has(h))throw Error('dead handle '+h);"
        "__procCalls.active.draws.push([h,i])},"
        "commit(){__procCalls.frames.push(__procCalls.active);__procCalls.active=null},"
        /* pocket.shapes' segments (shape_draw_shim.c). */
        "__seg(s){__procCalls.active.draws.push(['seg',s])}"
        "}}};";
    JSValue mock_result = JS_Eval(ctx, mock, sizeof mock - 1, "mock.js", JS_EVAL_TYPE_GLOBAL);
    REQUIRE(!JS_IsException(mock_result));
    JS_FreeValue(ctx, mock_result);
    /* WAKE draws through pocket.shapes; shape_draw_shim.c hands its draws to
     * the procedural mock above. */
    {
        JSValue g = JS_GetGlobalObject(ctx), pocket = JS_GetPropertyStr(ctx, g, "pocket");
        JSValue shapes = JS_NewObject(ctx);
        REQUIRE(pocket_shape_fill(ctx, shapes, NULL) == ESP_OK);
        JS_SetPropertyStr(ctx, pocket, "shapes", shapes);
        JS_FreeValue(ctx, pocket); JS_FreeValue(ctx, g);
        JSValue r = JS_Eval(ctx, SHAPE_DRAW_JS, sizeof SHAPE_DRAW_JS - 1, "shim.js", JS_EVAL_TYPE_GLOBAL);
        REQUIRE(!JS_IsException(r));
        JS_FreeValue(ctx, r);
    }
    /* argv[1]: the app's chunks, comma-separated, in apps/kasane/chunks.txt's
     * order (what the entry's pocket.app.load() calls evaluate). */
    char list[4096];
    snprintf(list, sizeof list, "%s", argv[1]);
    for (char *file = strtok(list, ","); file; file = strtok(NULL, ",")) {
        size_t source_size = 0;
        char *source = read_file(file, &source_size);
        JSValue result = JS_Eval(ctx, source, source_size, file, JS_EVAL_TYPE_GLOBAL);
        REQUIRE(!JS_IsException(result));
        JS_FreeValue(ctx, result);
        free(source);
    }
    JSValue global = JS_GetGlobalObject(ctx);
    /* Act I's former programs (the VM's reference workload, the last file
     * in the list: tools/kasane_contract/proc_megademo_news_ref.js). */
    JSValue demo = JS_GetPropertyStr(ctx, global, "procMegademoNews");
    REQUIRE(!JS_IsException(demo) && JS_IsObject(demo));

    unsigned programs = 0, layers_run = 0;
    for (unsigned phase = 0; phase < 3; ++phase) {
        for (unsigned layer = 0; layer < PROC_MEGA_LAYERS; ++layer) {
            ksn_proc_inst js_code[KSN_PROC_CODE], c_code[KSN_PROC_CODE];
            ksn_proc_program js_program, c_program;
            float c_input[KSN_PROC_INPUTS];
            read_program(ctx, demo, phase, layer, js_code, &js_program);
            REQUIRE(proc_mega_build(phase * 16, layer, c_code, &c_program, c_input));
            REQUIRE(js_program.count == c_program.count);
            for (unsigned i = 0; i < js_program.count; ++i) {
                const ksn_proc_inst *a = &js_code[i], *b = &c_code[i];
                REQUIRE(a->op == b->op && a->dst == b->dst && a->a == b->a &&
                        a->b == b->b && a->value == b->value && a->color == b->color);
            }
            REQUIRE(ksn_proc_plan_prepare(&js_plans[phase][layer], &js_program));
            REQUIRE(ksn_proc_plan_prepare(&c_plans[phase][layer], &c_program));
            ++programs;
        }
    }
    for (unsigned frame = 0; frame < PROC_MEGA_FRAMES; ++frame) {
        unsigned phase = proc_mega_phase(frame);
        uint16_t backdrop = read_backdrop(ctx, demo, frame);
        REQUIRE(backdrop == proc_mega_backdrop(frame));
        for (unsigned pixel = 0; pixel < KSN_PROC_W * KSN_PROC_H; ++pixel)
            js_pixels[pixel] = c_pixels[pixel] = backdrop;
        for (unsigned layer = 0; layer < PROC_MEGA_LAYERS; ++layer) {
            float js_input[KSN_PROC_INPUTS], c_input[KSN_PROC_INPUTS];
            ksn_proc_vm js_vm, c_vm;
            read_inputs(ctx, demo, frame, layer, js_input);
            REQUIRE(proc_mega_inputs(frame, layer, c_input));
            for (unsigned i = 0; i < KSN_PROC_INPUTS; ++i)
                REQUIRE(fabsf(js_input[i] - c_input[i]) <= 0.00001f);
            REQUIRE(ksn_proc_plan_begin(&js_vm, &js_plans[phase][layer],
                                        js_input, &js_frame) == KSN_PROC_RUNNING);
            REQUIRE(ksn_proc_plan_begin(&c_vm, &c_plans[phase][layer],
                                        c_input, &c_frame) == KSN_PROC_RUNNING);
            REQUIRE(ksn_proc_plan_run(&js_vm, &js_plans[phase][layer], false) == KSN_PROC_DONE);
            REQUIRE(ksn_proc_plan_run(&c_vm, &c_plans[phase][layer], false) == KSN_PROC_DONE);
            REQUIRE(ksn_proc_render_band(&js_frame, js_pixels, 0, KSN_PROC_H));
            REQUIRE(ksn_proc_render_band(&c_frame, c_pixels, 0, KSN_PROC_H));
            REQUIRE(memcmp(js_pixels, c_pixels, sizeof js_pixels) == 0);
            ++layers_run;
        }
    }
    /* Plans are registered per scene, the next scene's over the last frames
     * of the running one. The app opens with WAKE (104 frames, no plans:
     * Tama's backlit silhouette, drawn by pocket.shapes until frame 74, then
     * nothing), which prefetches Act I's six HUD plans; Act I's three scenes
     * (CALL 64, ROUTE 56, VOYAGE 96 frames) share them and draw the world
     * with pocket.shapes, and VOYAGE prefetches eight of TWIST's fifteen (the river
     * and the stars; its negative registers in TWIST). */
    static const char playback_check[] =
        /* Nothing is registered or allocated while the app evaluates
         * (docs/apps/megademo-story.md). */
        "if(__procCalls.registered.length!==0||typeof frame!=='function')"
        "throw Error('registration during evaluation');"
        "if(procMegademo.names[7]!=='WAKE'||procMegademo.len[7]!==104)throw Error('WAKE');"
        "for(let i=0;i<104;i++){frame();const f=__procCalls.frames[i];"
        "if(!f||(f.draws.length>0)!==(i<74))throw Error('WAKE frame '+i)}"
        "__procCalls.frames.length=0;"
        "frame();if(__procCalls.registered.length!==6||__procCalls.live.size!==6)"
        "throw Error('registration by the first Act I frame');"
        "for(const r of __procCalls.registered)if(r.batch!==undefined)throw Error('unexpected batch');"
        "for(let tick=1;tick<216;tick++)frame();"
        "if(__procCalls.frames.length!==216)throw Error('frame count');"
        "for(let tick=0;tick<216;tick++){const f=__procCalls.frames[tick];"
        "const plans=f.draws.filter(d=>d[0]!=='seg'),segs=f.draws.length-plans.length;"
        "if(f.backdrop!==(tick>204?0x0021:0x0827)||f.surface||!plans.length||tick>=120&&!segs)"
        "throw Error('Act I frame '+tick);"
        "for(const d of plans)if(d[0]<1||d[0]>6||!Array.isArray(d[1])||d[1].length>8)"
        "throw Error('Act I draw '+tick)}"
        "if(__procCalls.registered.length!==14)throw Error('TWIST prefetch '+__procCalls.registered.length);"
        /* The reference's Q14 point batches stay in bounds. */
        "for(let phase=0;phase<3;phase++){const b=procMegademoNews.pointBatch(phase,3);"
        "if(!b||b.kind!=='affineQ14Points'||b.x.length!==40||b.y.length!==40||"
        "b.coeff.length!==6||!Number.isInteger(b.color)||b.color<0||b.color>65535)"
        "throw Error('batch shape');"
        "for(let i=0;i<40;i++){"
        "if(!Number.isInteger(b.x[i])||!Number.isInteger(b.y[i])||"
        "b.x[i]<-32768||b.x[i]>32767||b.y[i]<-32768||b.y[i]>32767)"
        "throw Error('source point');"
        "const x=Math.floor((b.coeff[0]*b.x[i]+b.coeff[1]*b.y[i]+b.coeff[4])/16384);"
        "const y=Math.floor((b.coeff[2]*b.x[i]+b.coeff[3]*b.y[i]+b.coeff[5])/16384);"
        "if(x<0||x>=240||y<0||y>=135)throw Error('point bounds')}}";
    JSValue check = JS_Eval(ctx, playback_check, sizeof playback_check - 1,
                            "playback-check.js", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(check)) {
        JSValue e = JS_GetException(ctx);
        const char *m = JS_ToCString(ctx, e);
        fprintf(stderr, "playback check: %s\n", m ? m : "?");
    }
    REQUIRE(!JS_IsException(check));
    JS_FreeValue(ctx, check);
    /* The next frame is inside TWIST, at full screen (the story's camera went into
     * the screen during the news). Its set holds exactly two images: the
     * procedural image and the resized monitor. Leaving full screen rebuilds
     * the set with the room, so the images are looked up after each press. */
    static const char resize_check[] =
        "const rs=__procCalls.resize;"
        "if(rs.length!==1||rs[0].width!==112||rs[0].height!==63||rs[0].sampling!==undefined)"
        "throw Error('resize registration');"
        "const pick=w=>{const im=__procCalls.images;for(let i=im.length-1;i>=0;i--)"
        "if(im[i].spec.sourceWidth===w)return im[i]};"
        "if(!pick(240).visible||pick(112).visible)throw Error('initial images');"
        "frame(0x4000);for(let i=1;i<44;i++)frame(0);"
        "if(pick(240).visible||!pick(112).visible)throw Error('fixed monitor selection');"
        "frame(0x4000);"
        "if(!pick(240).visible||pick(112).visible)throw Error('dynamic zoom selection');"
        /* Two more loops: live plans never above 32, more than 32 in total. */
        "for(let i=0;i<800;i++)frame(0);"
        "if(__procCalls.peak>32||__procCalls.registered.length<=32||!__procCalls.unregistered)"
        "throw Error('loader '+__procCalls.peak+' '+__procCalls.registered.length);"
        "globalThis.__loader=[__procCalls.peak,__procCalls.registered.length];";
    check = JS_Eval(ctx, resize_check, sizeof resize_check - 1,
                    "resize-check.js", JS_EVAL_TYPE_GLOBAL);
    REQUIRE(!JS_IsException(check));
    JS_FreeValue(ctx, check);
    JS_FreeValue(ctx, demo);
    JS_FreeValue(ctx, global);
    JS_FreeContext(ctx);
    JS_FreeRuntime(runtime);
    printf("PASS JS megademo: %u Act I programs equal to the C reference, 3 bounded Q14 batches, %u reference frames, %u native plan layer runs, exact base RGB565 pixels, per-scene plan loading over 1134 playback calls\n",
           programs, PROC_MEGA_FRAMES, layers_run);
    return 0;
}
