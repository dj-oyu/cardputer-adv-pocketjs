// What the integer multiply, divide and shift instructions cost on THIS part.
//
// Shiki (docs/kasane/shiki-spec.md) is an integer machine, and its spec has
// to choose where a product may be 64 bits wide and where a division may
// appear. The numbers that choice turns on were nowhere: esp32s3-hw-mcp
// measured the PIE instructions only, the TRM prints no latency for the base
// ISA's MUL32/DIV32 options, and the only division price in this tree (~16
// cycles, docs/perf/pie-simd.md) is a back-calculation. A divider whose time
// depends on its operands, or a multiplier that is slower for 32-bit than for
// 16-bit values, would change the spec, so every case runs over several
// operand classes.
//
// The method is fpu_latency.c's. Each instruction P runs in three unrolled
// arms, from registers fixed here (a8 = x, a9 = y, SAR = z) so that the
// allocator is not what is measured:
//   thr  four independent P in a row (a10..a13): the issue cost.
//   dep  P, then "xor a11,a10,a10; or a8,a8,a11": the next P reads a8, which
//        now waits on P's result, yet keeps its value (x xor x is 0) so every
//        P sees the same operands. Minus the two ALU instructions (1 cycle
//        each, no stall between native ALU ops, fpu_latency.c's calibration),
//        this is P's result latency to an ALU consumer.
//   ind  the same three instructions with the chain cut: P's issue cost when
//        two instructions separate it from the next P.
// Times are the minimum of RUNS (a tick interrupt lands in some runs and an
// average keeps it), minus the same call with an empty body, per P.
//
// The 64-bit routines are timed in C: a loop that reads two volatile int64
// operands and folds the result into a sink, minus the same loop with an xor.
#include "int_latency.h"
#include "esp_cpu.h"
#include <stdint.h>
#include <stdio.h>

#define REPT  32
#define ITERS 100u
#define RUNS  7u

#define STR2(x) #x
#define STR(x)  STR2(x)
#define PROLOGUE "mov a8,%0\n mov a9,%1\n ssr %2\n"
#define CLOBBERS "a8","a9","a10","a11","a12","a13","memory"
#define ASM(body) __asm__ __volatile__(PROLOGUE ".rept " STR(REPT) "\n" body ".endr\n" \
                                       ::"r"(x),"r"(y),"r"(z):CLOBBERS)

typedef void (*arm_fn)(int32_t,int32_t,int32_t);
#define ARMS(nm,T) \
    static void nm##_thr(int32_t x,int32_t y,int32_t z){ASM(T(a10) T(a11) T(a12) T(a13));} \
    static void nm##_dep(int32_t x,int32_t y,int32_t z){ASM(T(a10) "xor a11,a10,a10\n or a8,a8,a11\n");} \
    static void nm##_ind(int32_t x,int32_t y,int32_t z){ASM(T(a10) "xor a11,a12,a12\n or a13,a13,a11\n");}

#define T_ADD(d)   "add " #d ",a8,a9\n"
#define T_MULL(d)  "mull " #d ",a8,a9\n"
#define T_MULSH(d) "mulsh " #d ",a8,a9\n"
#define T_MULUH(d) "muluh " #d ",a8,a9\n"
#define T_M16S(d)  "mul16s " #d ",a8,a9\n"
#define T_M16U(d)  "mul16u " #d ",a8,a9\n"
#define T_QUOS(d)  "quos " #d ",a8,a9\n"
#define T_QUOU(d)  "quou " #d ",a8,a9\n"
#define T_REMS(d)  "rems " #d ",a8,a9\n"
#define T_REMU(d)  "remu " #d ",a8,a9\n"
// The funnel shift with SAR set once (z), and the variable shift as a 64-bit
// shift by a register amount needs it: SAR written, then used at once.
#define T_SRC(d)   "src " #d ",a8,a9\n"
#define T_SSRC(d)  "ssr a9\n src " #d ",a8,a8\n"
ARMS(add,T_ADD) ARMS(mull,T_MULL) ARMS(mulsh,T_MULSH) ARMS(muluh,T_MULUH)
ARMS(m16s,T_M16S) ARMS(m16u,T_M16U) ARMS(quos,T_QUOS) ARMS(quou,T_QUOU)
ARMS(rems,T_REMS) ARMS(remu,T_REMU) ARMS(src,T_SRC) ARMS(ssrc,T_SSRC)
static void empty(int32_t x,int32_t y,int32_t z){
    __asm__ __volatile__(PROLOGUE::"r"(x),"r"(y),"r"(z):CLOBBERS);
}

static uint32_t run_arm(arm_fn f,int32_t x,int32_t y,int32_t z){
    uint32_t best=UINT32_MAX;
    for(unsigned r=0;r<RUNS;r++){
        const uint32_t t0=esp_cpu_get_cycle_count();
        for(unsigned i=0;i<ITERS;i++)f(x,y,z);
        const uint32_t dt=esp_cpu_get_cycle_count()-t0;
        if(dt<best)best=dt;
    }
    return best;
}

typedef struct { const char *name; arm_fn thr,dep,ind; unsigned per; int32_t x,y,z; } asm_case;
#define C(nm,label,per,x,y,z) {label,nm##_thr,nm##_dep,nm##_ind,per,(int32_t)(x),(int32_t)(y),(int32_t)(z)}
static const asm_case ASM_CASES[]={
    C(add,"add",1,1,2,0),                         /* the anchor: lat 1, thr 1 */
    C(mull,"mull",1,7,3,0),
    C(mull,"mull",1,0x7fff,0x7fff,0),
    C(mull,"mull",1,0x7fffffff,3,0),
    C(mull,"mull",1,3,0x7fffffff,0),
    C(mull,"mull",1,0x7fffffff,0x7fffffff,0),
    C(mull,"mull",1,-1,-1,0),
    C(mull,"mull",1,0x12345678,-0x7654321,0),
    C(mulsh,"mulsh",1,7,3,0),
    C(mulsh,"mulsh",1,0x7fffffff,0x7fffffff,0),
    C(mulsh,"mulsh",1,-1,-1,0),
    C(muluh,"muluh",1,7,3,0),
    C(muluh,"muluh",1,0xffffffff,0xffffffff,0),
    C(m16s,"mul16s",1,7,3,0),
    C(m16s,"mul16s",1,0x7fff,0x7fff,0),
    C(m16s,"mul16s",1,-0x8000,-0x8000,0),
    C(m16u,"mul16u",1,0xffff,0xffff,0),
    C(quos,"quos",1,7,3,0),
    C(quos,"quos",1,0x7fff,3,0),
    C(quos,"quos",1,0x7fffffff,3,0),
    C(quos,"quos",1,0x7fffffff,0x7fff,0),
    C(quos,"quos",1,0x7fffffff,0x7ffffff0,0),
    C(quos,"quos",1,3,0x7fffffff,0),
    C(quos,"quos",1,-0x7fffffff,3,0),
    C(quos,"quos",1,0x7fffffff,-1,0),
    C(quou,"quou",1,7,3,0),
    C(quou,"quou",1,0xffffffff,3,0),
    C(quou,"quou",1,0xffffffff,0xfffffff0,0),
    C(rems,"rems",1,7,3,0),
    C(rems,"rems",1,0x7fffffff,3,0),
    C(rems,"rems",1,0x7fffffff,0x7ffffff0,0),
    C(remu,"remu",1,0xffffffff,3,0),
    C(src,"src",1,0x12345678,0x9abcdef0,5),
    C(ssrc,"ssr+src",2,0x12345678,13,0),
};
#define ASM_COUNT (sizeof ASM_CASES/sizeof ASM_CASES[0])

// ------------------------------------------------- 64-bit routines in C
#define C64N 256u
static volatile int64_t va,vb;
static volatile int64_t vsink;
#define LOOP64(nm,expr) \
    static __attribute__((noinline)) uint32_t nm(void){ \
        uint32_t best=UINT32_MAX; \
        for(unsigned r=0;r<RUNS;r++){ \
            int64_t sink=0; \
            const uint32_t t0=esp_cpu_get_cycle_count(); \
            for(unsigned i=0;i<C64N;i++){const int64_t a=va,b=vb;sink^=(expr);} \
            const uint32_t dt=esp_cpu_get_cycle_count()-t0; \
            vsink=sink; if(dt<best)best=dt; } \
        return best; }
LOOP64(l_base,a^b)
LOOP64(l_div64s,a/b)
LOOP64(l_div64u,(int64_t)((uint64_t)a/(uint64_t)b))
LOOP64(l_mod64s,a%b)
LOOP64(l_mul64,a*b)
LOOP64(l_mul32w,(int64_t)(int32_t)a*(int32_t)b)
LOOP64(l_mulsh16,((int64_t)(int32_t)a*(int32_t)b)>>16)
LOOP64(l_shr64,a>>(b&63))
LOOP64(l_div32,(int64_t)((int32_t)a/(int32_t)b))
typedef struct { const char *name; uint32_t (*f)(void); int64_t a,b; } c64_case;
static const c64_case C64_CASES[]={
    {"div64s",l_div64s,100,7},
    {"div64s",l_div64s,((int64_t)1<<40)+123,1000003},
    {"div64s",l_div64s,INT64_MAX,3},
    {"div64s",l_div64s,((int64_t)1<<62)+5,((int64_t)1<<40)+7},
    {"div64s",l_div64s,-((int64_t)1<<50),12345},
    {"div64u",l_div64u,((int64_t)1<<40)+123,1000003},
    {"mod64s",l_mod64s,((int64_t)1<<40)+123,1000003},
    {"mul64",l_mul64,((int64_t)1<<40)+3,((int64_t)1<<20)+5},
    {"mul32w",l_mul32w,0x12345678,-0x7654321},
    {"mul32w>>16",l_mulsh16,0x12345678,-0x7654321},
    {"shr64",l_shr64,-((int64_t)1<<60),37},
    {"div32c",l_div32,0x7fffffff,3},
    {"div32c",l_div32,7,3},
};
#define C64_COUNT (sizeof C64_CASES/sizeof C64_CASES[0])

unsigned int_latency_count(void){return (unsigned)(ASM_COUNT+C64_COUNT);}
bool int_latency_case(unsigned i,char *out,size_t n){
    if(i<ASM_COUNT){
        const asm_case *c=&ASM_CASES[i];
        const double base=run_arm(empty,c->x,c->y,c->z);
        const double k=(double)REPT*ITERS;
        const double thr=(run_arm(c->thr,c->x,c->y,c->z)-base)/(k*4);
        const double dep=(run_arm(c->dep,c->x,c->y,c->z)-base)/k-2;
        const double ind=(run_arm(c->ind,c->x,c->y,c->z)-base)/k-2;
        // per>1 (ssr+src): the three columns are for the whole P.
        snprintf(out,n,"CB asm %s %ld %ld thr=%.2f lat=%.2f ind=%.2f per=%u",c->name,
                 (long)c->x,(long)c->y,thr,dep,ind,c->per);
        return true;
    }
    i-=ASM_COUNT;
    if(i<C64_COUNT){
        const c64_case *c=&C64_CASES[i];
        va=c->a;vb=c->b;
        const double base=l_base();
        const double cy=(c->f()-base)/C64N;
        snprintf(out,n,"CB c64 %s %lld %lld cy=%.2f",c->name,(long long)c->a,(long long)c->b,cy);
        return true;
    }
    return false;
}
