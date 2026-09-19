// What a dependent FPU instruction costs on THIS part.
//
// docs/perf/flower-shade.md 13 ends on a number that exists nowhere: the
// dependent-issue latency of the scalar FPU. esp32s3-hw-mcp measured the PIE
// instructions and nothing else, the TRM does not print pipeline latencies for
// the coprocessor, and three attempts to speed up flower.c's shade() were
// decided by that number without anyone having it. Two of them removed real
// work and moved nothing, which is what a latency-bound loop does.
//
// The method is the one experiments/pie-timing used: run N instructions that
// each consume the previous one's result, run N of the same instruction that do
// not, and take the difference. The dependent arm pays the pipeline; the
// independent arm pays the issue rate; the gap is the stall a scheduler has to
// fill. Reporting BOTH matters -- a 1.0 cy/instruction independent arm and a
// 4.0 dependent one is a completely different machine from 2.5 and 2.5, and
// only the first makes "put two chains side by side" a real optimisation.
//
// Every number here is cycles per instruction of the unrolled body. The C loop
// around it costs three instructions per REPT, which is why REPT is large; the
// minimum of several runs is reported because a tick interrupt lands in some of
// them and nothing removes it from an average.
#include "fpu_latency.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include <stdint.h>

#define REPT  128
#define ITERS 200u
#define RUNS  7u

// Each case is one macro pair. The registers are fixed rather than left to the
// compiler: the whole point is which operand of which instruction waits, and a
// constraint that let the allocator choose would be measuring the allocator.
// f1 holds 1.0f and f2 0.0f, so every arm's values stay put -- a chain of
// mul.s that ran to infinity or to a denormal would be timing the exponent
// range and not the pipeline.
#define STR2(x) #x
#define STR(x)  STR2(x)
#define BODY(asmstr) \
    __asm__ __volatile__(".rept " STR(REPT) "\n" asmstr "\n.endr\n" ::: \
        "f0","f3","f4","f5","f6","f7","a8","memory")

static uint32_t run_case(void (*body)(void)) {
    uint32_t best = UINT32_MAX;
    for (unsigned r = 0; r < RUNS; r++) {
        uint32_t t0 = esp_cpu_get_cycle_count();
        for (unsigned i = 0; i < ITERS; i++) body();
        uint32_t dt = esp_cpu_get_cycle_count() - t0;
        if (dt < best) best = dt;
    }
    return best;
}

// 1. mul.s, result of each feeding the next.
static void mul_dep(void)   { BODY("mul.s f0,f0,f1"); }
static void mul_indep(void) { BODY("mul.s f3,f7,f1\n mul.s f4,f7,f1\n mul.s f5,f7,f1\n mul.s f6,f7,f1"); }
// 2. madd.s accumulating into one register, against four independent ones.
static void madd_dep(void)   { BODY("madd.s f0,f1,f2"); }
static void madd_indep(void) { BODY("madd.s f3,f1,f2\n madd.s f4,f1,f2\n madd.s f5,f1,f2\n madd.s f6,f1,f2"); }
// 3. An FPU result crossing to a general register. shade does this three times
//    per pixel on the way into rgbd, and once in normal().
static void rfr_dep(void)   { BODY("mul.s f0,f0,f1\n rfr a8,f0"); }
static void rfr_indep(void) { BODY("mul.s f0,f0,f1\n rfr a8,f7"); }
// 4. The float-to-int conversion, which is how every colour channel leaves
//    shade. Note trunc.s writes a GENERAL register, so there is no rfr after
//    it -- the crossing is the instruction itself. The dependent arm converts a
//    value the previous instruction just produced.
static void trunc_dep(void)   { BODY("mul.s f0,f0,f1\n trunc.s a8,f0,0"); }
static void trunc_indep(void) { BODY("mul.s f0,f0,f1\n trunc.s a8,f7,0"); }
// 5. An FPU compare feeding a branch: POS() and every material test.
static void olt_dep(void)   { BODY("olt.s b0,f1,f2\n bt b0,1f\n 1:"); }
static void olt_indep(void) { BODY("olt.s b0,f1,f2\n bt b1,1f\n 1:"); }
// 6. A general register crossing INTO the FPU and being used at once, which is
//    what a literal-pool constant costs after l32r.
static void wfr_dep(void)   { BODY("wfr f3,a8\n mul.s f4,f3,f1"); }
static void wfr_indep(void) { BODY("wfr f3,a8\n mul.s f4,f7,f1"); }
// 7. The same value loaded straight into the FPU, which is what the material
//    table in shade does instead (docs/perf/flower-shade.md 9).
static void lsi_dep(void)   { BODY("lsi f3,a9,0\n mul.s f4,f3,f1"); }
static void lsi_indep(void) { BODY("lsi f3,a9,0\n mul.s f4,f7,f1"); }
// 8. Store then load at the SAME address, which is how shade's prologue
//    receives its two by-value vectors (flower-shade.md 13).
static void fwd_dep(void)   { BODY("s32i.n a8,a9,0\n lsi f3,a9,0"); }
static void fwd_indep(void) { BODY("s32i.n a8,a9,8\n lsi f3,a9,0"); }

// 9-10. How many INDEPENDENT CHAINS it takes to fill the stalls. One chain is
// case 2 at 4.07. These interleave two and three accumulators -- real chains,
// each operation waiting on the same register three back.
static void chain2(void) { BODY("madd.s f0,f1,f2\n madd.s f3,f1,f2"); }
static void chain3(void) { BODY("madd.s f0,f1,f2\n madd.s f3,f1,f2\n madd.s f4,f1,f2"); }

// 11-12. What hand-ordered assembly would buy, on the shape shade's lighting
// block actually has. One copy is two INDEPENDENT sub-chains -- the diffuse dot
// (three deep) and the specular dot raised to the sixteenth (seven deep) -- and
// this core is in-order, so the order the instructions are WRITTEN is the order
// they execute. Weaving is nothing but that order, which is precisely what a
// human can do and what the -O2 xtensa scheduler did not do inside shade
// (flower-shade.md's ablation: one extra chain there cost 7.3 cycles an
// operation, worse than serial).
//
//   naive1  one copy, each sub-chain written out whole, then the next
//   woven1  one copy, its two sub-chains interleaved
//   woven2  TWO copies, all four sub-chains interleaved
//
// If woven1 beats naive1, hand ordering pays inside a SINGLE pixel and no
// two-pixel refactor is needed at all. If woven2 costs what woven1 does, two
// pixels ride for the price of one.
static void naive1(void) { BODY(
    "mul.s f3,f0,f1\n madd.s f3,f0,f1\n madd.s f3,f0,f1\n"
    "mul.s f4,f0,f1\n madd.s f4,f0,f1\n madd.s f4,f0,f1\n"
    "mul.s f4,f4,f4\n mul.s f4,f4,f4\n mul.s f4,f4,f4\n mul.s f4,f4,f4\n"); }
static void woven1(void) { BODY(
    "mul.s f3,f0,f1\n mul.s f4,f0,f1\n"
    "madd.s f3,f0,f1\n madd.s f4,f0,f1\n"
    "madd.s f3,f0,f1\n madd.s f4,f0,f1\n"
    "mul.s f4,f4,f4\n mul.s f4,f4,f4\n mul.s f4,f4,f4\n mul.s f4,f4,f4\n"); }
static void woven2(void) { BODY(
    "mul.s f3,f0,f1\n mul.s f4,f0,f1\n mul.s f5,f0,f1\n mul.s f6,f0,f1\n"
    "madd.s f3,f0,f1\n madd.s f4,f0,f1\n madd.s f5,f0,f1\n madd.s f6,f0,f1\n"
    "madd.s f3,f0,f1\n madd.s f4,f0,f1\n madd.s f5,f0,f1\n madd.s f6,f0,f1\n"
    "mul.s f4,f4,f4\n mul.s f6,f6,f6\n mul.s f4,f4,f4\n mul.s f6,f6,f6\n"
    "mul.s f4,f4,f4\n mul.s f6,f6,f6\n mul.s f4,f4,f4\n mul.s f6,f6,f6\n"); }

// per_dep / per_indep are how many instructions each arm's REPT body holds.
// They differ: an independent arm needs several destination registers to break
// the chain, so it packs four instructions where the dependent one packs a
// single instruction. Dividing both by the same number -- which the first run
// of this probe did -- reports the independent arm as four times its cost and
// makes a real three-cycle stall look like none at all.
struct { const char *name; void (*dep)(void); void (*indep)(void);
         unsigned per_dep, per_indep; } static const
cases[] = {
    {"mul.s",          mul_dep,   mul_indep,   1,  4},
    {"madd.s",         madd_dep,  madd_indep,  1,  4},
    {"mul.s->rfr",     rfr_dep,   rfr_indep,   2,  2},
    {"mul.s->trunc",   trunc_dep, trunc_indep, 2,  2},
    {"olt.s->bt",      olt_dep,   olt_indep,   2,  2},
    {"wfr->mul.s",     wfr_dep,   wfr_indep,   2,  2},
    {"lsi->mul.s",     lsi_dep,   lsi_indep,   2,  2},
    {"s32i->lsi",      fwd_dep,   fwd_indep,   2,  2},
    {"chains 1v2",     madd_dep,  chain2,      1,  2},
    {"chains 1v3",     madd_dep,  chain3,      1,  3},
    // For these two the columns are "naive" and "woven"; the stall column is
    // what hand ordering recovers per instruction.
    {"lite naive|wov", naive1,    woven1,      10, 10},
    {"lite wov1|wov2", woven1,    woven2,      10, 20},
};

void fpu_latency_run(void) {
    // a9 points at four words of scratch for cases 7 and 8; f1 = 1.0f and
    // f2 = 0.0f so no arm drifts out of the normal range.
    static uint32_t scratch[4];
    scratch[0] = 0x3f800000u;
    __asm__ __volatile__(
        "wfr f1,%0\n wfr f2,%1\n wfr f7,%0\n wfr f0,%0\n mov.n a9,%2\n mov.n a8,%0\n"
        // b1 must be FALSE, like b0 is in the dependent arm: otherwise case 5
        // compares a taken branch against a not-taken one and reports the
        // branch, not the compare's latency. The first run of this probe had
        // it backwards and returned a negative stall, which is how it was
        // caught.
        "olt.s b1,f1,f2\n"
        :: "r"(0x3f800000u), "r"(0u), "r"(scratch) : "f0","f1","f2","f7","a8","a9");
    ESP_LOGI("fpu", "FPU_LATENCY rept=%u iters=%u runs=%u (cycles per instruction, "
                    "minimum of runs; dep = each consumes the previous result)",
             REPT, ITERS, RUNS);
    for (unsigned c = 0; c < sizeof(cases)/sizeof(cases[0]); c++) {
        double base = (double)REPT * (double)ITERS;
        double dep = run_case(cases[c].dep) / (base * cases[c].per_dep);
        double ind = run_case(cases[c].indep) / (base * cases[c].per_indep);
        ESP_LOGI("fpu", "FPU_CASE %-14s dep=%.2f indep=%.2f stall=%.2f cy/instr",
                 cases[c].name, dep, ind, dep - ind);
    }
    ESP_LOGI("fpu", "FPU_LATENCY_DONE");
}
