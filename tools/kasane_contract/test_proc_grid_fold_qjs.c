#include "quickjs.h"
#include "ksn_proc_grid_pie.h"
#include "ksn_proc_points.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(ok) do { if (!(ok)) { \
    fprintf(stderr, "grid fold QuickJS check failed at %s:%d\n", __FILE__, __LINE__); \
    exit(1); \
} } while (0)

static JSContext *js;
static FILE *trace_file;
static ksn_grid_pie_load_strategy trace_strategy;

static void trace_event(ksn_grid_trace_kind kind, const int16_t *a,
                        const int16_t *b, int32_t argument)
{
    static const char *const names[] = {
        "START_FIXED", "START_DYNAMIC", "TAP", "PAIR", "FUSED_PAIR", "STRIDE2",
        "BROADCAST_INPUT", "BROADCAST_COEFFICIENT", "DYNAMIC", "SCAN",
        "FINISH"
    };
    static const uint8_t a_count[] = {8, 0, 8, 16, 16, 16, 1, 8, 8, 8, 8};
    static const uint8_t b_count[] = {0, 0, 0, 0, 0, 0, 0, 1, 8, 8, 0};
    CHECK(trace_file && (unsigned)kind < sizeof names / sizeof names[0]);
    fprintf(trace_file, "%s %d", names[kind], argument);
    for (unsigned i = 0; i < a_count[kind]; ++i)
        fprintf(trace_file, " %d", a[i]);
    for (unsigned i = 0; i < b_count[kind]; ++i)
        fprintf(trace_file, " %d", b[i]);
    fputc('\n', trace_file);
}

static JSValue prop(JSValueConst object, const char *name)
{
    JSValue v = JS_GetPropertyStr(js, object, name);
    CHECK(!JS_IsException(v));
    return v;
}

static JSValue item(JSValueConst array, uint32_t at)
{
    JSValue v = JS_GetPropertyUint32(js, array, at);
    CHECK(!JS_IsException(v));
    return v;
}

static int64_t integer(JSValueConst v)
{
    int64_t n;
    CHECK(JS_IsNumber(v) && JS_ToInt64(js, &n, v) == 0);
    return n;
}

static int64_t field(JSValueConst object, const char *name)
{
    JSValue v = prop(object, name);
    int64_t n = integer(v);
    JS_FreeValue(js, v);
    return n;
}

static uint32_t length(JSValueConst array)
{
    CHECK(JS_IsArray(array));
    int64_t n = field(array, "length");
    CHECK(n >= 0 && n <= UINT32_MAX);
    return (uint32_t)n;
}

static ksn_grid_index read_index(JSValueConst array)
{
    CHECK(length(array) == 5);
    ksn_grid_index out = {0};
    for (unsigned i = 0; i < 5; ++i) {
        JSValue term = item(array, i);
        CHECK(length(term) == 3);
        JSValue a = item(term, 0), b = item(term, 1), c = item(term, 2);
        int64_t constant = integer(a), scale = integer(b), param = integer(c);
        CHECK(constant >= INT32_MIN && constant <= INT32_MAX &&
              scale >= INT16_MIN && scale <= INT16_MAX &&
              param >= 0 && param <= UINT8_MAX);
        out.term[i] = (ksn_grid_coeff){(int32_t)constant, (int16_t)scale,
                                      (uint8_t)param};
        JS_FreeValue(js, a); JS_FreeValue(js, b); JS_FreeValue(js, c);
        JS_FreeValue(js, term);
    }
    return out;
}

static ksn_grid_program read_program(JSValueConst spec, ksn_grid_shape *shape)
{
    ksn_grid_program p = {0};
    p.count = (uint8_t)field(spec, "count");
    p.result_reg = (uint8_t)field(spec, "result_reg");
    p.final_shift = (uint8_t)field(spec, "final_shift");
    p.initial = field(spec, "initial");
    CHECK(p.count && p.count <= KSN_GRID_CODE);
    JSValue body = prop(spec, "body");
    CHECK(length(body) == p.count);
    for (unsigned i = 0; i < p.count; ++i) {
        JSValue row = item(body, i);
        ksn_grid_instruction *in = &p.body[i];
        in->op = (uint8_t)field(row, "op");
        in->dst = (uint8_t)field(row, "dst");
        in->a = (uint8_t)field(row, "a");
        in->b = (uint8_t)field(row, "b");
        in->buffer = (uint8_t)field(row, "buffer");
        in->immediate = (int16_t)field(row, "immediate");
        JSValue at = prop(row, "index");
        in->index = read_index(at);
        JS_FreeValue(js, at); JS_FreeValue(js, row);
    }
    JS_FreeValue(js, body);
    JSValue output = prop(spec, "output");
    p.output = read_index(output);
    JS_FreeValue(js, output);
    JSValue dimensions = prop(spec, "shape");
    CHECK(length(dimensions) == 4);
    uint16_t *field_of_shape[] = {&shape->width, &shape->height,
                                  &shape->tap_width, &shape->tap_height};
    for (unsigned i = 0; i < 4; ++i) {
        JSValue v = item(dimensions, i);
        int64_t n = integer(v);
        CHECK(n > 0 && n <= UINT16_MAX);
        *field_of_shape[i] = (uint16_t)n;
        JS_FreeValue(js, v);
    }
    JS_FreeValue(js, dimensions);
    return p;
}

static size_t read_int16_array(JSValueConst array, int16_t out[1024])
{
    if (JS_IsUndefined(array)) return 0;
    uint32_t n = length(array);
    CHECK(n <= 1024);
    for (uint32_t i = 0; i < n; ++i) {
        JSValue v = item(array, i);
        int64_t value = integer(v);
        CHECK(value >= INT16_MIN && value <= INT16_MAX);
        out[i] = (int16_t)value;
        JS_FreeValue(js, v);
    }
    return n;
}

static void check_outputs(const ksn_grid_execution *e,
                          const int16_t *actual, const int16_t *expected)
{
    for (unsigned y = 0; y < e->shape.height; ++y)
        for (unsigned x = 0; x < e->shape.width; ++x) {
            size_t at = (size_t)(e->output[0] + e->output[1] * x +
                                 e->output[2] * y);
            CHECK(actual[at] == expected[y * e->shape.width + x]);
        }
}

static void run_example(JSValueConst example)
{
    JSValue label = prop(example, "name");
    const char *name = JS_ToCString(js, label);
    CHECK(name);
    JSValue spec = prop(example, "program");
    ksn_grid_shape shape = {0};
    ksn_grid_program program = read_program(spec, &shape);
    JS_FreeValue(js, spec);
    ksn_grid_plan plan;
    CHECK(ksn_grid_prepare(&program, &plan) == KSN_GRID_OK);
    JSValue expects_pie = prop(example, "pie");
    bool pie = JS_ToBool(js, expects_pie) == 1;
    JS_FreeValue(js, expects_pie);
    JSValue expects_scan = prop(example, "scanPie");
    bool scan_pie = JS_ToBool(js, expects_scan) == 1;
    JS_FreeValue(js, expects_scan);
    CHECK(plan.mac.valid == pie);
    JSValue buffers = prop(example, "buffers");
    int16_t source[1024] __attribute__((aligned(16))) = {0};
    int16_t weights[1024] __attribute__((aligned(16))) = {0};
    int16_t scalar[256] __attribute__((aligned(16))) = {0};
    int16_t optimized[256] __attribute__((aligned(16))) = {0};
    JSValue initial_dest = prop(example, "initialDest");
    if (!JS_IsUndefined(initial_dest)) {
        int16_t seed[1024] = {0};
        size_t n = read_int16_array(initial_dest, seed);
        CHECK(n <= 256);
        memcpy(scalar, seed, n * sizeof(int16_t));
        memcpy(optimized, seed, n * sizeof(int16_t));
    }
    JS_FreeValue(js, initial_dest);
    JSValue input = prop(buffers, "0"), coeff = prop(buffers, "2");
    ksn_grid_binding binding = {0};
    binding.count[0] = read_int16_array(input, source);
    binding.count[2] = read_int16_array(coeff, weights);
    binding.data[0] = source;
    binding.data[2] = weights;
    binding.data[KSN_GRID_DEST] = scalar;
    binding.count[KSN_GRID_DEST] = 256;
    JS_FreeValue(js, input); JS_FreeValue(js, coeff); JS_FreeValue(js, buffers);
    JSValue params = prop(example, "params");
    if (!JS_IsUndefined(params)) {
        CHECK(length(params) <= KSN_GRID_PARAMS);
        for (unsigned i = 0; i < length(params); ++i) {
            JSValue v = item(params, i);
            int64_t n = integer(v);
            CHECK(n >= INT32_MIN && n <= INT32_MAX);
            binding.param[i] = (int32_t)n;
            JS_FreeValue(js, v);
        }
    }
    JS_FreeValue(js, params);
    JSValue expected_js = prop(example, "expected");
    CHECK(length(expected_js) == (uint32_t)shape.width * shape.height);
    int16_t expected[1024] = {0};
    size_t output_count = read_int16_array(expected_js, expected);
    JS_FreeValue(js, expected_js);
    CHECK(output_count <= 256);
    ksn_grid_execution execution;
    CHECK(ksn_grid_begin(&plan, &shape, &binding, &execution) == KSN_GRID_OK);
    CHECK(execution.scan_rows_candidate == scan_pie);
    CHECK(ksn_grid_run_scalar(&execution) == KSN_GRID_OK);
    check_outputs(&execution, scalar, expected);
    binding.data[KSN_GRID_DEST] = optimized;
    CHECK(ksn_grid_begin(&plan, &shape, &binding, &execution) == KSN_GRID_OK);
    CHECK(ksn_grid_pie_eligible(&execution) == pie);
    CHECK(ksn_grid_scan_pie_eligible(&execution) == scan_pie);
    const ksn_grid_pie_policy policy = {true, 8, 8, 8};
    if (trace_file) fprintf(trace_file, "EXAMPLE %s %s\n", name,
                            pie ? "PIE" : scan_pie ? "SCAN" : "SCALAR");
    if (trace_file && pie) {
        uint8_t candidates = ksn_grid_pie_candidates(&execution);
        execution.requested_strategy =
            (candidates & (1u << trace_strategy)) ? trace_strategy :
            (candidates & (1u << KSN_GRID_PIE_LOAD_AFFINE)) ?
            KSN_GRID_PIE_LOAD_AFFINE : KSN_GRID_PIE_LOAD_GATHER;
    }
    CHECK(ksn_grid_run_auto(&execution, &policy) == KSN_GRID_OK);
    CHECK(execution.pie_backend_selected == (pie || scan_pie));
    check_outputs(&execution, optimized, expected);
    CHECK(memcmp(scalar, optimized, sizeof(scalar)) == 0);
    if (trace_file) fprintf(trace_file, "END %s\n", name);
    if (strcmp(name, "affineX") == 0 || strcmp(name, "affineY") == 0) {
        CHECK(output_count == 40 && binding.count[0] == 80);
        int16_t x[40], y[40], out_x[40], out_y[40];
        for (unsigned i = 0; i < 40; ++i) {
            x[i] = source[2 * i];
            y[i] = source[2 * i + 1];
        }
        KsnProcAffineQ14 coeffs = {15872, 2048, -1024, 15360,
                                   225000, -327680};
        ksn_proc_points_affine_scalar((KsnProcPointDst){out_x, out_y},
                                      (KsnProcPointSrc){x, y}, 40, &coeffs);
        CHECK(memcmp(expected, strcmp(name, "affineX") == 0 ? out_x : out_y,
                     output_count * sizeof(int16_t)) == 0);
    }
    printf("grid JS->IR->%s: %s body=%u outputs=%zu taps=%u\n",
           scan_pie ? "scan PIE model" : pie ? "PIE model" : "scalar",
           name, plan.program.count,
           output_count, shape.tap_width * shape.tap_height);
    JS_FreeCString(js, name); JS_FreeValue(js, label);
}

static void evaluate(const char *path)
{
    FILE *file = fopen(path, "rb");
    CHECK(file && fseek(file, 0, SEEK_END) == 0);
    long size = ftell(file);
    CHECK(size > 0 && fseek(file, 0, SEEK_SET) == 0);
    char *source = malloc((size_t)size + 1);
    CHECK(source && fread(source, 1, (size_t)size, file) == (size_t)size);
    CHECK(fclose(file) == 0);
    source[size] = 0;
    JSValue result = JS_Eval(js, source, (size_t)size, path, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue error = JS_GetException(js);
        const char *message = JS_ToCString(js, error);
        fprintf(stderr, "grid fold QuickJS: %s\n", message ? message : "exception");
        if (message) JS_FreeCString(js, message);
        JS_FreeValue(js, error);
        exit(1);
    }
    JS_FreeValue(js, result);
    free(source);
}

int main(int argc, char **argv)
{
    CHECK(argc == 3 || argc == 4 || argc == 5);
    if (argc >= 4) {
        trace_file = fopen(argv[3], "wb");
        CHECK(trace_file);
        g_ksn_grid_pie_trace = trace_event;
        /* Keep the affine templates covered even when AUTO chooses the
         * measured faster gather route for 2x2 windows. */
        trace_strategy = argc == 5 &&
            strcmp(argv[4], "fused") == 0 ? KSN_GRID_PIE_LOAD_FUSED :
            KSN_GRID_PIE_LOAD_AFFINE;
    }
    JSRuntime *runtime = JS_NewRuntime();
    CHECK(runtime);
    js = JS_NewContext(runtime);
    CHECK(js);
    evaluate(argv[1]);
    evaluate(argv[2]);
    JSValue global = JS_GetGlobalObject(js);
    JSValue examples = prop(global, "gridFoldExamples");
    uint32_t count = length(examples);
    CHECK(count == 25);
    for (unsigned i = 0; i < count; ++i) {
        JSValue example = item(examples, i);
        run_example(example);
        JS_FreeValue(js, example);
    }
    JS_FreeValue(js, examples); JS_FreeValue(js, global);
    JS_FreeContext(js); JS_FreeRuntime(runtime);
    if (trace_file) {
        CHECK(fclose(trace_file) == 0);
        trace_file = NULL;
        g_ksn_grid_pie_trace = NULL;
        trace_strategy = KSN_GRID_PIE_LOAD_AUTO;
    }
    puts("grid fold QuickJS: all examples passed");
    return 0;
}
