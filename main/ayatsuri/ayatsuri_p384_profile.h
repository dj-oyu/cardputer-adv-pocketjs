#pragma once
#include <stdbool.h>
#include <stdint.h>

/* One synchronous caller. Multiple scopes may append to the same phase. */
void ay_p384_set_detail(bool detailed);
void ay_p384_set_table(bool enabled);
void ay_p384_set_modexp(bool enabled);
bool ay_p384_modexp_selftest(void);
void ay_p384_note_modexp_start(void);
bool ay_p384_detail(void);
void ay_p384_scope_begin(bool detailed);
void ay_p384_scope_end(void);
void ay_p384_report(const char *phase);

enum ay_p384_field_op { AY_P384_MUL, AY_P384_SQR, AY_P384_REDUCE, AY_P384_ADD,
                        AY_P384_SUB, AY_P384_INVERT, AY_P384_FIELD_OPS };
/* Returns -1 outside an armed point call or inside another counted field op. */
int64_t ay_p384_field_begin(void);
void ay_p384_field_end(enum ay_p384_field_op op, int64_t start);
