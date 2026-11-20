#pragma once
#include <stdbool.h>
#include "sdkconfig.h"

#if CONFIG_AYATSURI_CERT_INV_SCRATCH
bool ay_cert_inv_scratch_selftest(void);
void ay_cert_inv_scratch_begin(bool enabled);
unsigned ay_cert_inv_scratch_end(void);
#else
static inline bool ay_cert_inv_scratch_selftest(void) { return false; }
static inline void ay_cert_inv_scratch_begin(bool enabled) { (void)enabled; }
static inline unsigned ay_cert_inv_scratch_end(void) { return 0; }
#endif
