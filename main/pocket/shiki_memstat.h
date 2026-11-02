#ifndef SHIKI_MEMSTAT_H
#define SHIKI_MEMSTAT_H
/* What the native side of the Kasane line/grid surfaces (the future Shiki
 * heap, docs/kasane/shiki.md sec.4.5) allocates, per call site: a build
 * option (CONFIG_POCKET_SHIKI_MEMSTAT, default off) and never in the product
 * image. A file opts in by defining SHIKI_MS_FILE (a shiki_ms_file value)
 * and including this header after <stdlib.h>: its malloc/calloc/free then go
 * through the counters, which record the heap block size the allocator
 * really took (heap_caps_get_allocated_size). app_stop() reports once per
 * session ("SHIKI_MEM ..." lines) and resets after the teardown. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#include <stddef.h>

enum shiki_ms_file { SHIKI_MS_PROC, SHIKI_MS_GRID, SHIKI_MS_DERBY, SHIKI_MS_FILES };

#ifdef CONFIG_POCKET_SHIKI_MEMSTAT
void *shiki_ms_malloc(int file,int line,size_t n);
void *shiki_ms_calloc(int file,int line,size_t c,size_t n);
void shiki_ms_free(void *p);
void shiki_memstat_report(void);
void shiki_memstat_reset(void);
#ifdef SHIKI_MS_FILE
#define malloc(n) shiki_ms_malloc(SHIKI_MS_FILE,__LINE__,(n))
#define calloc(c,n) shiki_ms_calloc(SHIKI_MS_FILE,__LINE__,(c),(n))
#define free(p) shiki_ms_free(p)
#endif
#else
static inline void shiki_memstat_report(void){}
static inline void shiki_memstat_reset(void){}
#endif
#endif
