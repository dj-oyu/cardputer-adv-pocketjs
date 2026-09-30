/* Runs two programs of the procedural VM (main/ui/kasane/ksn_procedural.c)
 * on the same inputs and compares everything a draw makes visible: status,
 * segments (ends and colour, in order) and raster steps. Host only.
 *
 *   run_ir < cases.txt
 *
 * cases.txt (tools/kasane_ir/check_equivalence.py writes it):
 *   CASE name tag
 *   A n        then n rows "op dst a b value color" (the hand IR, assembled)
 *   B n        then n rows (the compiled IR, assembled)
 *   I v0 .. v7 one line per input vector (as many as wanted)
 *   END
 * Both run through the reference VM (ksn_proc_begin/run); B also through
 * the registered plan (ksn_proc_plan_prepare/run), which pocket_proc.c uses.
 * Prints one line per case: vectors, mismatches, statuses, executed steps.
 */
#include "ksn_proc_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ksn_proc_frame fa, fb, fp;
static ksn_proc_vm va, vb, vp;
static ksn_proc_plan plan;

static int read_code(ksn_proc_inst *code, unsigned n) {
    for (unsigned i = 0; i < n; i++) {
        unsigned op, dst, a, b, color; double value;
        if (scanf("%u %u %u %u %lf %u", &op, &dst, &a, &b, &value, &color) != 6) return 0;
        code[i] = (ksn_proc_inst){(uint8_t)op, (uint8_t)dst, (uint8_t)a, (uint8_t)b, (float)value, (uint16_t)color};
    }
    return 1;
}
static int same(const ksn_proc_frame *x, const ksn_proc_frame *y) {
    if (x->count != y->count || x->raster_steps != y->raster_steps) return 0;
    return !memcmp(x->segments, y->segments, x->count * sizeof x->segments[0]);
}
int main(void) {
    char word[64], name[64], tag[64];
    unsigned failures = 0;
    while (scanf("%63s", word) == 1) {
        if (strcmp(word, "CASE")) { fprintf(stderr, "expected CASE, got %s\n", word); return 2; }
        if (scanf("%63s %63s", name, tag) != 2) return 2;
        static ksn_proc_inst A[KSN_PROC_CODE], B[KSN_PROC_CODE];
        unsigned na, nb;
        if (scanf(" A %u", &na) != 1 || na > KSN_PROC_CODE || !read_code(A, na)) return 2;
        if (scanf(" B %u", &nb) != 1 || nb > KSN_PROC_CODE || !read_code(B, nb)) return 2;
        const ksn_proc_program pa = {A, (uint8_t)na}, pb = {B, (uint8_t)nb};
        const int prepared = ksn_proc_plan_prepare(&plan, &pb);
        unsigned vectors = 0, bad = 0, done = 0, failed = 0, first_bad = 0;
        unsigned long steps_a = 0, steps_b = 0, segs = 0;
        for (;;) {
            if (scanf("%63s", word) != 1) return 2;
            if (!strcmp(word, "END")) break;
            float in[KSN_PROC_INPUTS];
            for (unsigned j = 0; j < KSN_PROC_INPUTS; j++) { double v; if (scanf("%lf", &v) != 1) return 2; in[j] = (float)v; }
            vectors++;
            ksn_proc_status sa = ksn_proc_begin(&va, &pa, in, &fa);
            if (sa == KSN_PROC_RUNNING) sa = ksn_proc_run(&va);
            ksn_proc_status sb = ksn_proc_begin(&vb, &pb, in, &fb);
            if (sb == KSN_PROC_RUNNING) sb = ksn_proc_run(&vb);
            ksn_proc_status sp = prepared ? ksn_proc_plan_begin(&vp, &plan, in, &fp) : KSN_PROC_INVALID;
            if (sp == KSN_PROC_RUNNING) sp = ksn_proc_plan_run(&vp, &plan, false);
            int ok = sa == sb && sb == sp && (sa != KSN_PROC_DONE || (same(&fa, &fb) && same(&fb, &fp)));
            if (!ok && !bad) first_bad = vectors;
            bad += !ok;
            if (sa == KSN_PROC_DONE) { done++; steps_a += va.steps; steps_b += vb.steps; segs += fa.count; }
            else failed++;
        }
        failures += bad;
        printf("%-8s %-10s vectors %5u  mismatches %u%s  done %u  failed %u  segments %lu  steps hand %lu compiled %lu\n",
               name, tag, vectors, bad, bad ? (sprintf(word, " (first #%u)", first_bad), word) : "", done, failed, segs,
               steps_a, steps_b);
    }
    printf(failures ? "EQUIVALENCE FAIL\n" : "EQUIVALENCE PASS\n");
    return failures ? 1 : 0;
}
