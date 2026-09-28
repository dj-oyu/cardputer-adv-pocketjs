#include "quickjs.h"
#include "pocket_grid.h"
#include "pocket_api.h"
#include "ui/kasane/ksn_proc_grid_resize.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "grid app test failed %s:%d: %s\n", __FILE__, __LINE__, #x); \
    exit(1); \
} } while (0)

static ksn_image_port image;
static ksn_image_port resize_image;
static ksn_image_port stream_image;
static ksn_image_port nearest_image;
static unsigned invalidates, resources;
static unsigned source_reads, source_pixels, source_version;
static int fail_source_y = -1;
const char *pocket_grid_test_frontend;

JSValue pocket_kasane_grid_resource(JSContext *ctx, unsigned slot,
                                    const ksn_image_port *port)
{
    CHECK(port);
    if (slot == 0) {
        CHECK(port->width == 16 && port->height == 12);
        image = *port;
    } else if (slot == 1) {
        CHECK(port->width == 42 && port->height == 21);
        resize_image = *port;
    } else if (slot == 2) {
        CHECK(port->width == 112 && port->height == 63);
        stream_image = *port;
    } else {
        CHECK(slot == 3 && port->width == 112 && port->height == 63);
        nearest_image = *port;
    }
    ++resources;
    return JS_NewObject(ctx);
}
void pocket_kasane_grid_invalidate(unsigned slot)
{
    CHECK(slot <= 3);
    ++invalidates;
}
static uint16_t source_pixel(unsigned x, unsigned y)
{
    return (uint16_t)((((x * 7 + y * 11 + source_version) & 31) << 11) |
                      (((x * 9 + y * 13 + source_version) & 63) << 5) |
                      ((x * 17 + y * 3 + source_version) & 31));
}
static ksn_result source_span(void *ctx, uint16_t variant, uint16_t frame,
                              uint16_t y, uint16_t x, uint16_t count,
                              uint16_t *rgb565, uint8_t *alpha)
{
    (void)ctx;
    if (variant || frame || y >= 135 || x > 240 || count > 240 - x)
        return KSN_INVALID;
    if ((int)y == fail_source_y) return KSN_IO;
    ++source_reads;
    source_pixels += count;
    for (unsigned i = 0; i < count; ++i) {
        rgb565[i] = source_pixel(x + i, y);
        alpha[i] = 255;
    }
    return KSN_OK;
}
bool pocket_kasane_grid_source_port(JSContext *ctx, JSValueConst object,
                                    ksn_image_port *out, uint32_t *id)
{
    JSValue tag = JS_GetPropertyStr(ctx, object, "tag");
    const char *name = JS_ToCString(ctx, tag);
    bool valid = name && !strcmp(name, "host-source");
    if (name) JS_FreeCString(ctx, name);
    JS_FreeValue(ctx, tag);
    if (!valid) return false;
    *id = 777;
    *out = (ksn_image_port){.width=240,.height=135,.variants=1,.frames=1,
        .read_span=source_span,.opaque=true};
    return true;
}
JSValue pocket_api_throw(JSContext *ctx, const char *code, const char *op,
                         const char *message, bool retryable,
                         const char *outcome)
{
    (void)retryable; (void)outcome;
    return JS_ThrowTypeError(ctx, "%s %s: %s", code, op, message);
}

static char *read_file(const char *name)
{
    FILE *file = fopen(name, "rb"); CHECK(file);
    CHECK(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file); CHECK(length > 0);
    CHECK(fseek(file, 0, SEEK_SET) == 0);
    char *text = malloc((size_t)length + 1); CHECK(text);
    CHECK(fread(text, 1, (size_t)length, file) == (size_t)length);
    CHECK(fclose(file) == 0);
    text[length] = 0;
    return text;
}
static void eval(JSContext *ctx, const char *source, bool expect_error)
{
    JSValue result = JS_Eval(ctx, source, strlen(source), "grid-app.js",
                             JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result) && !expect_error) {
        JSValue error = JS_GetException(ctx);
        const char *message = JS_ToCString(ctx, error);
        fprintf(stderr, "grid JS exception: %s\n", message ? message : "unknown");
        if (message) JS_FreeCString(ctx, message);
        JS_FreeValue(ctx, error);
    }
    CHECK(JS_IsException(result) == expect_error);
    if (expect_error) {
        JSValue error = JS_GetException(ctx);
        JS_FreeValue(ctx, error);
    }
    JS_FreeValue(ctx, result);
}

static void pixels(unsigned add)
{
    uint16_t row[16]; uint8_t alpha[16];
    for (unsigned y = 0; y < 12; ++y) {
        CHECK(image.read_span(image.ctx, 0, 0, y, 0, 16, row, alpha) == KSN_OK);
        for (unsigned x = 0; x < 16; ++x) {
            unsigned sx = x * 2, sy = y * 2;
            unsigned sum = 0;
            for (unsigned ty = 0; ty < 2; ++ty)
                for (unsigned tx = 0; tx < 2; ++tx)
                    sum += (((sy + ty) * 32 + sx + tx) * 7 + add) & 255;
            CHECK(row[x] == (uint16_t)(sum >> 2));
            CHECK(alpha[x] == 255);
        }
    }
}

int main(int argc, char **argv)
{
    CHECK(argc == 3);
    JSRuntime *runtime = JS_NewRuntime(); CHECK(runtime);
    JSContext *ctx = JS_NewContext(runtime); CHECK(ctx);
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue ns = JS_NewObject(ctx);
    CHECK(pocket_grid_install(ctx, ns) == ESP_OK);
    CHECK(JS_SetPropertyStr(ctx, global, "kasane", ns) == 1);
    JS_FreeValue(ctx, global);
    char *frontend = read_file(argv[1]), *program = read_file(argv[2]);
    pocket_grid_test_frontend = frontend;
    eval(ctx, program, false);
    free(frontend); free(program);
    eval(ctx, "globalThis.h=kasane.grid.register(gridFoldDeviceProgram);"
              "let registered=kasane.grid.registration(h);"
              "if(registered.irCount!==gridFoldDeviceProgram.count||"
              "registered.planBytes<registered.analysisBytes||"
              "registered.macTerms!==1||registered.checkedOps<1||"
              "registered.totalUs<registered.prepareUs||"
              "registered.heapAfter>registered.heapBefore)"
              "throw Error('registration');"
              "globalThis.input=new Int16Array(768);"
              "for(let i=0;i<input.length;i++)input[i]=(i*7)&255;"
              "if(kasane.grid.run(h,{0:input},[32,16])!=='PIE')throw Error('backend');"
              "let route=kasane.grid.explain(h);"
              "if(route.backend!=='PIE'||route.kernel!=='MAC'||"
              "route.scalarReason!=='NONE'||route.candidateMask===0||"
              "route.strategy!=='FUSED'||"
              "route.reason!=='PROFILE')throw Error(JSON.stringify(route));"
              "let measure=kasane.grid.measure(h,2);"
              "if(!measure.equal||measure.repeats!==2||"
              "measure.scalarUs<0||measure.pieUs<0)throw Error('measure');"
              "for(const strategy of ['GATHER','AFFINE']){"
              "let forced=kasane.grid.measure(h,2,strategy);"
              "if(!forced.equal||forced.strategy!==strategy||forced.pieUs<0)"
              "throw Error('forced measure');}"
              "globalThis.resource=kasane.grid.resource(h);", false);
    CHECK(resources == 1 && !pocket_grid_pending());
    pixels(0);
    eval(ctx, "for(let i=0;i<input.length;i++)input[i]=(i*7+4)&255;"
              "if(kasane.grid.run(h,{0:input},[32,16])!=='PIE')throw Error('backend');",
         false);
    CHECK(pocket_grid_pending() && invalidates == 1);
    pixels(4);
    eval(ctx, "kasane.grid.run(h,{0:input},[32,16])", true);
    pocket_grid_present_result(KSN_IO);
    CHECK(pocket_grid_pending());
    pixels(4);
    pocket_grid_present_result(KSN_OK);
    CHECK(!pocket_grid_pending());
    eval(ctx, "kasane.grid.run(h,{0:new Uint8Array(768)},[32,16])", true);
    pixels(4);
    eval(ctx, "kasane.grid.run(h,{0:input},[32,16]);", false);
    CHECK(pocket_grid_pending() && invalidates == 2);
    pocket_grid_present_result(KSN_OK);
    eval(ctx, "globalThis.hr=kasane.grid.registerResize({sourceWidth:90,"
              "sourceHeight:45,width:42,height:21});"
              "globalThis.rgb=new Int16Array(90*45);"
              "for(let i=0;i<rgb.length;i++)rgb[i]=0x1234;"
              "if(kasane.grid.run(hr,{0:rgb})!=='PIE')throw Error('resize backend');"
              "let r=kasane.grid.explain(hr);"
              "if(r.strategy!=='BILINEAR'||r.reason!=='EXPERIMENT')"
              "throw Error(JSON.stringify(r));"
              "globalThis.rr=kasane.grid.resource(hr);", false);
    CHECK(resources == 2 && !pocket_grid_pending());
    eval(ctx, "kasane.grid.measure(hr)", true);
    uint16_t row[42]; uint8_t alpha[42];
    CHECK(resize_image.read_span(resize_image.ctx, 0, 0, 0, 0, 42,
                                 row, alpha) == KSN_OK);
    for (unsigned i = 0; i < 42; ++i) CHECK(row[i] == 0x1234);
    eval(ctx, "rgb.fill(0x7bef);kasane.grid.run(hr,{0:rgb});", false);
    CHECK(pocket_grid_pending() && invalidates == 3);
    CHECK(resize_image.read_span(resize_image.ctx, 0, 0, 20, 0, 42,
                                 row, alpha) == KSN_OK);
    for (unsigned i = 0; i < 42; ++i) CHECK(row[i] == 0x7bef);
    pocket_grid_present_result(KSN_IO);
    CHECK(pocket_grid_pending());
    pocket_grid_present_result(KSN_OK);
    CHECK(!pocket_grid_pending());
    eval(ctx, "globalThis.hs=kasane.grid.registerResizeSource({"
              "source:{tag:'host-source'},width:112,height:63});"
              "globalThis.rs=kasane.grid.resource(hs);"
              "if(kasane.grid.explain(hs).backend!=='PIE')throw Error('stream');", false);
    CHECK(resources == 3 && !pocket_grid_pending());
    ksn_grid_resize_plan plan;
    CHECK(ksn_grid_resize_prepare_stream(&plan, 240, 135, 112, 63));
    uint16_t source0[240], source1[240], actual[112], expected[112];
    uint8_t stream_alpha[112];
    for (unsigned y = 0; y < 63; ++y) {
        unsigned sy = plan.y0[y], sy1 = sy + (sy + 1 < 135);
        for (unsigned x = 0; x < 240; ++x) {
            source0[x] = source_pixel(x, sy);
            source1[x] = source_pixel(x, sy1);
        }
        CHECK(ksn_grid_resize_span(&plan, y, 0, 112, source0, source1,
                                   expected, false, NULL));
        /* Different span boundaries must give exactly the same pixels. */
        CHECK(stream_image.read_span(stream_image.ctx, 0, 0, y, 0, 37,
                                     actual, stream_alpha) == KSN_OK);
        CHECK(stream_image.read_span(stream_image.ctx, 0, 0, y, 37, 75,
                                     actual + 37, stream_alpha + 37) == KSN_OK);
        for (unsigned x = 0; x < 112; ++x) {
            CHECK(actual[x] == expected[x]);
            CHECK(stream_alpha[x] == 255);
        }
    }
    CHECK(source_reads == 126);
    ++source_version;
    pocket_grid_source_invalidated(777);
    CHECK(invalidates == 4);
    CHECK(stream_image.read_span(stream_image.ctx, 0, 0, 62, 0, 112,
                                 actual, stream_alpha) == KSN_OK);
    CHECK(source_reads == 128);
    for (unsigned x = 0; x < 240; ++x) {
        source0[x] = source_pixel(x, plan.y0[62]);
        source1[x] = source_pixel(x, plan.y0[62] + 1);
    }
    CHECK(ksn_grid_resize_span(&plan, 62, 0, 112, source0, source1,
                               expected, false, NULL));
    for (unsigned x = 0; x < 112; ++x) CHECK(actual[x] == expected[x]);
    CHECK(stream_image.read_span(stream_image.ctx, 0, 0, 0, 0, 112,
                                 actual, stream_alpha) == KSN_OK);
    fail_source_y = (int)plan.y0[20] + 1;
    CHECK(stream_image.read_span(stream_image.ctx, 0, 0, 20, 0, 112,
                                 actual, stream_alpha) == KSN_IO);
    fail_source_y = -1;
    CHECK(stream_image.read_span(stream_image.ctx, 0, 0, 0, 0, 112,
                                 actual, stream_alpha) == KSN_OK);
    for (unsigned x = 0; x < 240; ++x) {
        source0[x] = source_pixel(x, plan.y0[0]);
        source1[x] = source_pixel(x, plan.y0[0] + 1);
    }
    CHECK(ksn_grid_resize_span(&plan, 0, 0, 112, source0, source1,
                               expected, false, NULL));
    for (unsigned x = 0; x < 112; ++x) CHECK(actual[x] == expected[x]);
    eval(ctx, "kasane.grid.registerResizeSource({source:{tag:'host-source'},"
              "width:112,height:63,sampling:'linear'})", true);
    eval(ctx, "globalThis.hn=kasane.grid.registerResizeSource({"
              "source:{tag:'host-source'},width:112,height:63,"
              "sampling:'nearest'});globalThis.rn=kasane.grid.resource(hn);"
              "let n=kasane.grid.explain(hn);"
              "if(n.strategy!=='NEAREST'||n.reason!=='EXPLICIT'||"
              "n.backend!=='scalar')throw Error(JSON.stringify(n));",
         false);
    CHECK(resources == 4 && nearest_image.read_span);
    source_reads=0;source_pixels=0;
    for(unsigned y=0;y<63;y++)for(unsigned x=0;x<112;x+=16){
        CHECK(nearest_image.read_span(nearest_image.ctx,0,0,y,x,16,
                actual,stream_alpha)==KSN_OK);
        unsigned sy=(y*135u+135u/2u)/63u;
        if(sy>=135)sy=134;
        for(unsigned i=0;i<16;i++){
            unsigned sx=((x+i)*240u+120u)/112u;
            if(sx>=240)sx=239;
            CHECK(actual[i]==source_pixel(sx,sy));
            CHECK(stream_alpha[i]==255);
        }
    }
    CHECK(source_reads==441);
    printf("nearest source stream: %u spans, %u source pixels\n",
           source_reads,source_pixels);
    fail_source_y=(int)((20u*135u+67u)/63u);
    CHECK(nearest_image.read_span(nearest_image.ctx,0,0,20,0,16,
            actual,stream_alpha)==KSN_IO);
    fail_source_y=-1;
    CHECK(nearest_image.read_span(nearest_image.ctx,0,0,20,0,16,
            actual,stream_alpha)==KSN_OK);
    unsigned before_invalidates=invalidates;
    pocket_grid_source_invalidated(777);
    CHECK(invalidates==before_invalidates+2);
    eval(ctx, "kasane.grid.run(hs,{})", true);
    eval(ctx, "let broken={...gridFoldDeviceProgram,body:"
              "gridFoldDeviceProgram.body.map(row=>({...row}))};"
              "broken.body[0].dst=8;let detail='';"
              "try{kasane.grid.register(broken)}catch(error){detail=String(error)}"
              "if(!detail.includes('body[0]')||"
              "!detail.includes('invalid destination register'))"
              "throw Error('IR diagnostic '+detail);"
              "let at=gridFold.index({x:1,y:8});"
              "let slow=gridFold.fold({width:8,height:1,tapWidth:1,"
              "tapHeight:1,output:at},g=>g.add(g.acc,g.min("
              "g.load(0,at),g.constant(0))));"
              "let slowHandle=kasane.grid.register(slow);"
              "if(kasane.grid.registration(slowHandle).macTerms!==0)"
              "throw Error('form diagnostic');"
              "if(kasane.grid.run(slowHandle,{0:new Int16Array(8).fill(-1)})"
              "!=='scalar')throw Error('scalar route');"
              "let slowRoute=kasane.grid.explain(slowHandle);"
              "if(slowRoute.kernel!=='SCALAR'||"
              "slowRoute.scalarReason!=='GENERAL_FORM'||"
              "slowRoute.candidateMask!==0)"
              "throw Error(JSON.stringify(slowRoute));", false);
    pocket_grid_reset();
    eval(ctx, "let narrow=gridFold.index({x:1,y:7});"
              "let narrowPlan=gridFold.fold({width:7,height:1,tapWidth:1,"
              "tapHeight:1,output:narrow},g=>g.add(g.acc,g.load(0,narrow)));"
              "let narrowHandle=kasane.grid.register(narrowPlan);"
              "if(kasane.grid.run(narrowHandle,{0:new Int16Array(7).fill(1)})"
              "!=='scalar')throw Error('narrow route');"
              "let narrowRoute=kasane.grid.explain(narrowHandle);"
              "if(narrowRoute.kernel!=='SCALAR'||"
              "narrowRoute.scalarReason!=='VECTOR_LAYOUT'||"
              "narrowRoute.candidateMask!==0)"
              "throw Error(JSON.stringify(narrowRoute));"
              "let wide=gridFold.index({x:1,y:8});"
              "let rangePlan=gridFold.fold({width:8,height:1,tapWidth:1,"
              "tapHeight:1,output:wide},g=>{"
              "let sample=g.load(0,wide);let square=g.mul(sample,sample);"
              "return g.add(g.acc,g.mul(square,square));});"
              "let rangeHandle=kasane.grid.register(rangePlan);"
              "if(kasane.grid.run(rangeHandle,{0:new Int16Array(8).fill(1)})"
              "!=='scalar')throw Error('range route');"
              "let rangeRoute=kasane.grid.explain(rangeHandle);"
              "if(rangeRoute.kernel!=='SCALAR'||"
              "rangeRoute.scalarReason!=='QACC_RANGE'||"
              "rangeRoute.candidateMask!==0)"
              "throw Error(JSON.stringify(rangeRoute));", false);
    pocket_grid_reset();
    JS_FreeContext(ctx); JS_FreeRuntime(runtime);
    puts("grid fold, arbitrary resize and source-stream QuickJS->PIE->image passed");
    return 0;
}
