#ifndef POCKET_SHIKI_FRAME_H
#define POCKET_SHIKI_FRAME_H
#include <stdbool.h>
#include "quickjs.h"
#include "esp_err.h"
#include "ui/kasane/ksn_ports.h"
#include "ui/kasane/ksn_shiki_frame.h"

/* The frames, surfaces and image resources Shiki draws into (the procedural
 * IR that first used them went in v2.0). beginFrame, commit, resource,
 * createSurface on obj (kasane.shiki, pocket_shiki_api.c). */
esp_err_t pocket_proc_frame_install(JSContext *ctx, JSValueConst obj);
void pocket_proc_reset(void);
/* Shiki programs' slots, handles and frames
 * (pocket_shiki_api.c): attach takes ownership of plan (freed by detach or
 * reset) and returns its handle, 0 when the 32 slots or the handles are
 * spent; draw runs it into the frame beginFrame() opened. */
uint32_t pocket_proc_shiki_attach(void *plan);
void *pocket_proc_shiki_plan(uint32_t handle);
bool pocket_proc_shiki_detach(uint32_t handle);
JSValue pocket_proc_shiki_draw(JSContext *ctx,uint32_t handle,const double *in,unsigned n);
/* The same with the inputs either as JS numbers (in) or, when fixed is not
 * NULL, already in each input's Q format (shiki.drawMany with an Int32Array),
 * and the draw's number, what DRAW_INDEX reads (drawMany's entry; 0 for
 * draw()). */
JSValue pocket_proc_shiki_draw_inputs(JSContext *ctx,uint32_t handle,const double *in,const int32_t *fixed,unsigned n,
                                      unsigned index);
/* Diagnostic (shikiDebug.drawProfile, docs/kasane/shiki-bundle.md): CPU cycles
 * a Shiki draw spends in each native stage, summed while on. parse: js_draw's
 * argument checks and input reads; lookup: from there to the engine (handle,
 * frame, workspace); engine: ksn_shiki_draw; copy: the segments into the
 * frame. mark is the cycle count where the next stage starts. Off by default:
 * one branch per stage. pocket_cycles() is 0 off the device. */
typedef struct { bool on; uint32_t n,parse,lookup,engine,copy,mark; } pocket_shiki_draw_prof;
extern pocket_shiki_draw_prof pocket_shiki_prof;
/* Always on (a few cycle reads per draw): what the Shiki draws of the turns
 * since the last take cost, for the SHIKI_TURN line next to KASANE_PAINT
 * (docs/kasane/shiki-jit.md sec.9). engine: the conversion and the engine;
 * total: the whole native draw (lookup, engine, copy into the frame). */
typedef struct { uint32_t draws,steps,engine_cy,total_cy; } pocket_shiki_turn;
void pocket_shiki_turn_take(pocket_shiki_turn *out);
uint32_t pocket_cycles(void);
void pocket_proc_end_turn(void);
/* One draw for a native caller (pocket_derby.c): a Shiki handle and its
 * inputs as numbers, with shiki.draw()'s checks and PocketError. Returns
 * JS_UNDEFINED or JS_EXCEPTION. */
JSValue pocket_proc_draw_numbers(JSContext *ctx,uint32_t handle,const double *in,unsigned n);
#ifdef POCKET_PROC_DRAW_HOOK
/* Host harness only: called after each native draw that succeeded, with the
 * inputs as given; with pocket_proc_hook_dry set, the draw is not run at all
 * (the harness compares input lists without frames or plans). */
void pocket_proc_draw_hook(uint32_t handle,const double *in,unsigned n);
extern bool pocket_proc_hook_dry;
#endif
/* Native drawing into the frame being built (pocket_shape.c): segments go
 * straight into the candidate under draw()'s frame limits -- the surface's
 * segment count and 65,535 raster steps -- and coordinate range (-480..720),
 * without a plan or the VM. begin() checks beginFrame and reentrancy and
 * marks the candidate; a failing line() abandons the frame as a failing
 * draw() does (LIMIT_EXCEEDED / INVALID_ARGUMENT, beginFrame again), and the
 * caller rolls back to the mark so the candidate keeps no part of the call.
 * Each returns JS_UNDEFINED or JS_EXCEPTION. */
typedef struct { uint16_t count,raster; uint8_t ext,surface; } pocket_proc_mark;
JSValue pocket_proc_native_begin(JSContext *ctx,const char *op,pocket_proc_mark *mark);
JSValue pocket_proc_native_line(JSContext *ctx,const char *op,int x0,int y0,int x1,int y1,uint16_t color);
void pocket_proc_native_rollback(const pocket_proc_mark *mark);
/* The 8-row bands (17) of the image a just-committed frame changes, and on
 * each its columns [x0, x1): the bands whose ordered entries differ from the
 * frame on the panel (all of them without one, or with a new backdrop
 * colour). False outside commit(). pocket_kasane.c turns it into image
 * damage instead of commit()'s one bounding rectangle. */
bool pocket_proc_band_damage(unsigned surface,uint32_t *bands,int16_t x0[],int16_t x1[]);
// Takes what a present drawn in strip_rows pieces needs, before it runs.
void pocket_proc_prepare_render(unsigned strip_rows);
bool pocket_proc_has_frame(void);
bool pocket_proc_pending(void);
bool pocket_proc_backdrop_pending(void);
/* Session-scoped dynamic image. Its descriptor borrows the adapter until reset. */
void pocket_proc_image_port(ksn_image_port *out);
void pocket_proc_image_port_at(ksn_image_port *out,unsigned surface);
void pocket_proc_image_mode(void);
void pocket_proc_image_mode_at(unsigned surface);
bool pocket_proc_is_image_mode(void);
ksn_result pocket_proc_backdrop(void *ctx,uint16_t y,uint16_t rows,uint16_t *pixels);
/* Called only after the native presenter attempts a pending procedural frame. */
void pocket_proc_present_result(ksn_result result);
/* Native FLOWER backdrop presentation can ACK only procedural image nodes. */
void pocket_proc_overlay_present_result(ksn_result result);
#ifdef KASANE_MEGADEMO_TRACE
/* Per-turn costs for the MEGADEMO trace (app_session.c), read and cleared
 * once per host turn. Times are esp_timer microseconds except band_cy. */
typedef struct {
    uint32_t reg_n,reg_us,prep_us,prep_max_us,unreg_n;
    uint32_t draw_n,draw_us,draw_max_us,band_n,band_cy;
    /* commit(), replace()/patch(), and panel presents made while a commit
     * had no view update after it yet: the picture that pairs a new image
     * with the previous frame's view tree. */
    uint32_t commit_n,view_n,split_n;
} pocket_proc_trace;
void pocket_proc_trace_take(pocket_proc_trace *out);
void pocket_proc_trace_view(void);
void pocket_proc_trace_presented(void);
#endif
#ifdef KASANE_PROC_JS_DIAGNOSTIC
void pocket_proc_image_prof_read(uint32_t *band_count,uint32_t *band_cycles,
                                 uint32_t *span_count,uint32_t *span_cycles);
#endif
#endif
