/* Private PocketJS integration; not a serialized format or a public VM ABI.
 * One synchronous top-level call, generated from this engine's own bytecode.
 * The descriptor borrows bytecode only while JS_EvalFunction owns its frame.
 */
#ifndef QUICKJS_PREPARED_H
#define QUICKJS_PREPARED_H
#include "quickjs.h"
#define PJS_PREPARED_REGISTER_MAGIC 0x5141
#define PJS_PREPARED_PARAMS 8
typedef struct {
    const void *bytecode;
    unsigned lookup_pc, next_pc, argc, params;
    char name[64];
    double numbers[PJS_PREPARED_PARAMS];
    float args[PJS_PREPARED_PARAMS];
} PJSPreparedCall;
/* Allocation-free, bounded recognition of one direct literal call. */
bool pjs_prepare_literal_call(JSContext *,JSValueConst,PJSPreparedCall *);
/* Weak defaults in the engine; the procedural adapter owns the binding and
 * synchronous scope. Dispatch false MUST leave argv and exceptions untouched.
 * Called after ordinary native stack checks/frame/realm/argv setup. */
void pjs_prepared_binding_created(JSContext *,JSValueConst,const void *,unsigned);
bool pjs_prepared_dispatch(JSContext *,JSValueConst,JSValueConst,int,
                          JSValueConst *,const void *,unsigned,JSValue *);
#endif
