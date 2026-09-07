// Does the bell's bounding-cylinder rejection ever drop a hit, and how much
// does it drop?
//
// bell_hit costs about 2,750 cycles a visit on the device -- six latitude bands
// walked unconditionally, each with a square root and a pair of divisions --
// and it is 52% of ray_row, rising to 66% on the bell-heavy species. Most of
// those visits miss. bell_reject is a fourteen-operation test that throws a
// visit out before the band loop when the ray's closest approach to the bell's
// axis is wider than the bell's widest radius.
//
// It is conservative by construction: it ignores the -1..1 height limit, so the
// set it rejects is a subset of the misses. "By construction" is not a
// measurement, so this program builds flower.c with FLOWER_BELL_CHECK, which
// computes the rejection without acting on it and runs the full walk anyway,
// and counts every visit where the test said no and the walk said yes. That
// count must be zero.
//
//   gcc -O2 -Wall -Wextra -Werror -DFLOWER_BELL_CHECK tools/test_bell_reject.c -lm -o /tmp/br && /tmp/br
#include "../main/scene/scene_mem.c"
#include "../main/scene/garden.c"
#include "../main/scene/flower.c"
#include <stdio.h>
#include <assert.h>

#define PHASES 24
static uint16_t fb[W*H];
static const char *NAME[8]={"CRYSTAL","VALLEY","SUNFLOWER","SNOWDROP",
                            "TULIP","DAFFODIL","CROCUS","CALLA"};

int main(void) {
    unsigned long all_seen=0,all_rej=0,all_wrong=0;
    for(int sp=0;sp<8;sp++) {
        bell_visits_seen=bell_rejected=bell_rejected_wrongly=0;
        for(int ph=0;ph<PHASES;ph++) {
            elapsed=1.3f+ph*0.83f-1.0f/30;
            flower_prepare(1.0f/30,0,0,(flower_species_t)sp);
            flower_draw(fb,0,H);
        }
        if(bell_visits_seen)
            printf("%-10s %7u bell visits, %7u rejected (%5.1f%%), %u wrongly\n",
                   NAME[sp],bell_visits_seen,bell_rejected,
                   100.0*bell_rejected/bell_visits_seen,bell_rejected_wrongly);
        else
            printf("%-10s no bell parts\n",NAME[sp]);
        all_seen+=bell_visits_seen;all_rej+=bell_rejected;
        all_wrong+=bell_rejected_wrongly;
        // The proof. Anything but zero means the test is not conservative and
        // the silhouette has holes in it.
        assert(bell_rejected_wrongly==0);
    }
    printf("\nBELL_REJECT_OK: %lu/%lu bell visits rejected (%.1f%%), %lu hits dropped,"
           " over 8 species x %d phases\n",
           all_rej,all_seen,all_seen?100.0*all_rej/all_seen:0,all_wrong,PHASES);
    assert(all_wrong==0);
    // Worth having as a floor: a test that rejects nothing is safe and useless,
    // and would mean the bound is not tight enough to be worth its fourteen
    // operations.
    assert(all_seen==0||all_rej*100>all_seen*20);
    return 0;
}
