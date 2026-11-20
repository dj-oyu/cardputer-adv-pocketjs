#pragma once

#include "sdkconfig.h"

#if CONFIG_AYATSURI_CERT_HEAP_TRACE
void ay_cert_heap_trace_begin(void);
void ay_cert_heap_trace_end(void);
void ay_cert_heap_trace_report(const char *phase);
#else
static inline void ay_cert_heap_trace_begin(void) {}
static inline void ay_cert_heap_trace_end(void) {}
static inline void ay_cert_heap_trace_report(const char *phase) { (void)phase; }
#endif
