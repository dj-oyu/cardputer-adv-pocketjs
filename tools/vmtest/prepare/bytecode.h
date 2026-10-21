#ifndef PREPARE_BYTECODE_H
#define PREPARE_BYTECODE_H
#include "quickjs.h"
#include <stdbool.h>
#define PREP_STACK 64
#define PREP_CALLS 32
#define PREP_FUNCTIONS 16
#define PREP_NUMBERS 8
/* Abstract facts, never a replacement execution stack or runtime argv. */
typedef enum {P_UNKNOWN,P_PATH,P_STRING,P_NUMBER,P_VECTOR,P_EMPTY} prep_kind;
typedef struct {
    prep_kind kind;
    unsigned length;
    union {unsigned path;double number;double numbers[PREP_NUMBERS];char text[64];} u;
} prep_value;
typedef struct {
    unsigned function,pc,argc;
    bool path_candidate;
    prep_value args[3];
} prep_call;
typedef struct {
    prep_call calls[PREP_CALLS];
    unsigned count,functions;
    bool bounded_fallback;
    size_t scratch_bytes;
} prep_report;
/* Uses the engine's compiled bytecode, never source token/name matching. */
bool prep_analyze(JSContext *,JSValueConst compiled,prep_report *);
/* Private-engine host probe only. No new firmware/VM public ABI. */
bool prep_native_identity(JSContext *,JSValueConst,JSCFunction *);
#endif
