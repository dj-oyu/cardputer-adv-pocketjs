#pragma once
#include <stdbool.h>

/* Single synchronous native HTTPS caller, begin/end on the same task. */
void ayatsuri_https_hw_begin(void);
void ayatsuri_https_hw_end(const char *phase);
bool ayatsuri_https_hw_short_mul_selftest(void);
void ayatsuri_https_hw_set_short_mul(bool enabled);
void ayatsuri_https_hw_use_cert_inv_scratch(bool enabled);
