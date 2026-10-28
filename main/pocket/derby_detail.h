#pragma once
#include <stdint.h>
#include <stddef.h>
/* Owner-task elapsed time (including scheduling/preemption), never bus time.
 * Logical writes/copies are not physical bus traffic. No ISR stack access. */
enum dd_stage { DD_JS_FRAME, DD_SIMULATION, DD_POSE_PROJECTION, DD_SORT, DD_RUNNER, DD_HUD, DD_NATIVE_SERIES, DD_PROC_BUILD, DD_VM_SETUP, DD_VM_ZERO, DD_VM_COPY, DD_VM_VALIDATE, DD_VM_MATCH, DD_VM_RUN, DD_SEGMENT_APPEND, DD_RENDER, DD_RENDER_PREPARE, DD_BACKDROP, DD_COMPOSITE, DD_IMAGE_SPAN, DD_BAND_RASTER, DD_SPAN_COPY, DD_TX_SYNC, DD_TX_COPY, DD_TX_SUBMIT, DD_TX_REAP, DD_STAGES };
enum dd_event { DD_BRIDGE_SNAPSHOT_BYTES, DD_VM_ZERO_BYTES, DD_VM_CODE_COPY_BYTES, DD_SEGMENT_COPY_BYTES, DD_BAND_CLEAR_BYTES, DD_SPAN_RGB_BYTES, DD_SPAN_ALPHA_BYTES, DD_SHARED_OUTPUT_BYTES, DD_TX_COPY_BYTES, DD_TX_PAYLOAD_BYTES, DD_TX_SUBMITS, DD_TX_REAPS, DD_TX_FAILURES, DD_EVENTS };
typedef struct { uint64_t inclusive_us,self_us,calls; } dd_counter;
typedef struct { dd_counter stage[DD_STAGES]; uint64_t event[DD_EVENTS],faults,pairs; } dd_snapshot;
#ifdef POCKET_DERBY_DETAIL_MEASURE
#ifndef POCKET_DERBY_MEASURE
#error Detail measurement requires coarse measurement
#endif
typedef struct { unsigned depth; int active; } dd_scope;
dd_scope dd_enter(enum dd_stage stage);
void dd_leave(dd_scope *scope);
void dd_add(enum dd_event event,uint64_t amount);
void dd_start(void);
void dd_cancel(void);
int dd_finish(dd_snapshot *out);
void dd_suspend(void);
void dd_resume(void);
#define DD_SUSPEND() dd_suspend()
#define DD_RESUME() dd_resume()
void dd_js(int stage); /* negative closes one JS scope; C scopes remain nested */
#define DD_CAT_(a,b) a##b
#define DD_CAT(a,b) DD_CAT_(a,b)
#define DD_OPEN(name,stage) dd_scope name __attribute__((cleanup(dd_leave)))=dd_enter(stage)
#define DD_CLOSE(name) dd_leave(&name)
#define DD_SCOPE(stage) DD_OPEN(DD_CAT(dd_scope_,__LINE__),stage)
#define DD_ADD(event,amount) dd_add(event,amount)
#else
#define DD_SUSPEND() ((void)0)
#define DD_RESUME() ((void)0)
#define DD_OPEN(name,stage) ((void)0)
#define DD_CLOSE(name) ((void)0)
#define DD_SCOPE(stage) ((void)0)
#define DD_ADD(event,amount) ((void)0)
#endif
