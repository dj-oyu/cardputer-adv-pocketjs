#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
enum df_counter { DF_COURSE,DF_VIEW,DF_SER,DF_CLIP,DF_SQRT,DF_HIT,DF_MISS,DF_REFRESH,DF_BATCH,DF_ROWS,DF_COUNTERS };
enum df_pace { DF_PACE_CONT,DF_PACE_FLOOR,DF_PACE_CAP,DF_PACES };
enum df_phase { DF_OFF,DF_READY,DF_ARMED,DF_MEASURE,DF_TAIL,DF_DONE,DF_ABORT };
enum df_error { DF_IDENTITY=1,DF_STATE=2,DF_CONTEXT=4,DF_MEASUREMENT=8,DF_CAPTURE=16,
                DF_OUTPUT=32,DF_CLOCK=64,DF_TIMEOUT=128,DF_INPUT=256,DF_PRESENT=512 };
#ifdef POCKET_DERBY_FIXED_WORKLOAD
void df_reset(void);
bool df_session(void);
bool df_measuring(void);
unsigned df_status(void);
void df_command(unsigned command,unsigned rep,unsigned step,uint32_t seed,unsigned camera,unsigned tier,uint32_t finish);
void df_dispatch(bool continuing,bool failed,uint32_t buttons);
void df_present(bool completed,bool capture,bool failed);
void df_owner_begin(uint64_t now);
void df_owner_work_end(uint64_t now);
void df_pace_begin(enum df_pace kind,uint64_t now,uint32_t requested_ticks);
void df_pace_end(uint64_t now);
void df_owner_end(uint64_t now,bool running);
void df_count(enum df_counter counter,uint64_t amount);
void df_fail(unsigned reason);
/* Fixed platform boundary, supplied by device adapter or deterministic host. */
uint64_t df_now_us(void);
int df_write(const char *data,size_t size);
const char *df_variant(void);
#define DF_COUNT(c) df_count(c,1)
#define DF_ADD(c,n) df_count(c,n)
#define DF_OWNER_BEGIN(t) df_owner_begin(t)
#define DF_WORK_END(t) df_owner_work_end(t)
#define DF_PACE_BEGIN(k,t,n) df_pace_begin(k,t,n)
#define DF_PACE_END(t) df_pace_end(t)
#define DF_OWNER_END(t,r) df_owner_end(t,r)
#else
#define DF_COUNT(c) ((void)0)
#define DF_ADD(c,n) ((void)0)
#define DF_OWNER_BEGIN(t) ((void)0)
#define DF_WORK_END(t) ((void)0)
#define DF_PACE_BEGIN(k,t,n) ((void)0)
#define DF_PACE_END(t) ((void)0)
#define DF_OWNER_END(t,r) ((void)0)
#endif
