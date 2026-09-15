// Byte-identity harness for the garden row pipeline, decor pass included.
//
//   gcc -O2 tools/flower_decor_identity.c -lm -o /tmp/id_new && /tmp/id_new > /tmp/d_new
//   (in a checkout of the previous revision)  ... && /tmp/id_old > /tmp/d_old
//   cmp /tmp/d_new /tmp/d_old
//
// The decor optimisation inside garden_decor_row is a claim about which profile
// evaluations can be skipped, so the only acceptable evidence is "the same
// pixels". Every path is exercised: mix=0 (one vegetation layout), mix in
// 1..255 (two layouts and the dissolve), mix>=256 and old_seed==seed (the plain
// garden_row path), over 120 frames of animation.
#include "../main/scene/garden.c"
#include <stdio.h>

// Only used when the harness is built with -DGARDEN_COUNT_PROFILE: how many
// profile evaluations the gate removes, which is the number that explains why
// the timing did or did not move. Printed on stderr so stdout stays byte-exact.
unsigned long g_profile_calls;

int main(void) {
    static uint16_t fb[135 * 240];
    GardenFrame g = {0};
    // GATE0=1 runs the pre-2026-09-15 path, so both arms can be compared
    // against the previous revision and not just the shipping one.
    if (getenv("GATE0")) g_garden_decor_gate = 0;
    for (int w = 0; w < 16; w++) garden_prepare(&g, w * 0.04f);
    for (int fr = 0; fr < 120; fr++) {
        garden_prepare(&g, fr * 0.37f);
        unsigned mix = (fr % 8 == 0) ? 256u : (fr % 8 == 4) ? 0u : (unsigned)(fr * 37) % 256u;
        unsigned old_seed = (fr & 1) ? (g.seed ^ 0x9e3779b9u) : g.seed;
        for (int y = 0; y < 135; y++) {
            uint16_t row[240];
            for (int x = 0; x < 240; x++) row[x] = fb[y * 240 + x];
            garden_row_blend(row, y, &g, old_seed, mix);
            for (int x = 0; x < 240; x++) fb[y * 240 + x] = row[x];
        }
        fwrite(fb, sizeof fb, 1, stdout);
    }
#ifdef GARDEN_COUNT_PROFILE
    fprintf(stderr, "gate=%d profile_calls=%lu\n", g_garden_decor_gate, g_profile_calls);
#endif
    return 0;
}
