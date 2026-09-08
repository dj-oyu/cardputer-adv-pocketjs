#pragma once
#include <stdint.h>

// Midges in the shaft. Fourteen of them, and the number is the point: enough to
// read as a swarm, few enough that the eye can follow one.
//
// The motion is modelled on insects rather than on dust, which means the
// heading is random-walked and the speed is not. A midge flies at a fairly
// steady rate and changes direction abruptly and often; random-walking the
// position instead gives a jitter that reads as noise, and a constant velocity
// gives dust. It holds a volume rather than crossing the frame, so each carries
// a home point and a weak tether to it, and it hovers -- the speed drops to
// nothing for a few frames now and then, which is most of the character.
//
// A particle that leaves the shaft is destroyed and replaced. That is the whole
// of the lifetime rule, and it needs no boundary logic: outside the shaft the
// light that makes a particle visible is zero, so following one there would be
// work spent on something nobody can see. It also makes the swarm refresh
// itself, because a midge darting near the edge will eventually cross it.
//
// The home is an offset from the shaft rather than a fixed point on the screen,
// so the swarm rides the beam as it sways instead of being culled by it. Only
// the midges own wandering ends a life, which is the behaviour that was asked
// for; a beam that moves out from under a stationary swarm is not.
//
// `age` exists only to fade the first few frames, so a replacement does not
// appear as a blink.
// Two deaths, deliberately asymmetric, and the asymmetry is the point rather
// than an oversight:
//
//   leaving the shaft is instant. Outside the beam q is zero, so the particle
//   is already invisible -- there is nothing to animate and nobody to show it
//   to. The cull test and the visibility test are the same fact.
//
//   crossing into the bottom fifth is gradual. That happens inside the lit
//   area where it can be watched, so it needs a visible process instead of a
//   disappearance.
//
// The second is a one-way latch, not a region test. A particle hovering on the
// line would otherwise flicker between fading and not, which is the single most
// bug-looking thing this could do; a latch has no hysteresis to tune and no
// oscillation to find later.
//
// `dir` carries the heading and that latch in one byte. Magnitude 1..127 is the
// heading -- 127 of them, 2.8 degrees apart, far finer than anything visible on
// a mote three pixels across -- and the sign is whether it is dying. They
// cannot drift apart because they are the same byte, and the sign flips exactly
// once in a life. Magnitude is never zero, because zero has no sign and the
// latch would be unreadable there; -128 never occurs for the same reason from
// the other end.
//
// Inverting the sign also reverses the heading, which is the nudge: a dying
// midge turns and withdraws the way it came, while the random walk keeps
// running on top so it still reads as an insect and not as something on rails.
typedef struct {
    int16_t x,y;        /* position, 1/16 px -- it moves less than a pixel a frame */
    int16_t hoff,hy;    /* the home it holds: an offset from the shaft, and a row */
    int8_t  dir;        /* |dir|-1 is the heading; dir < 0 means dying */
    uint8_t speed;      /* 1/16 px a frame; zero means never seeded */
    uint8_t glow;       /* peak addition to `sun`, before the shaft gates it */
    uint8_t age;        /* frames since spawn, saturating: the ramp that hides a birth */
    uint8_t dim;        /* frames of dying left, once the latch has fired */
} GardenMote;
#define GARDEN_DYING 24     /* a second at 24 fps: long enough to read as leaving */
#define GARDEN_DOOM  108    /* the bottom fifth of 135 */
#define GARDEN_MOTES 14
#define GARDEN_ROWS 135
// Which particles touch which row, decided once a frame instead of once a row.
//
// IT SAVES SIXTY MICROSECONDS. That is the measurement, and it is the reason
// this comment exists in this form rather than claiming a win.
//
//   GARDEN_MOTE_INDEX=1   pixels 6.80 ms
//   GARDEN_MOTE_INDEX=0   pixels 6.86 ms   (mean of 11 samples)
//
// Removing 1,863 of 1,890 row-particle tests moves the frame by 0.06 ms. Each
// test costs about eight cycles, not the 127 that motivated building this.
//
// The 1.0 ms this was meant to remove came from comparing two different builds
// with the swarm absent from one, and i-cache alignment moves the same kernel
// by 15% between builds -- 0.9 ms of a 6 ms pass. The figure was inside its own
// noise. It is recorded here because a number that turned out to be an artefact
// is worth more written down than deleted: the same comparison would produce it
// again.
//
// So this is not an optimisation. It is a correct, asserted, zero-cost way of
// answering the question, which is worth keeping for a swarm ten times larger
// and is worth nothing today. What it must not do is claim a saving it does
// not have.
//
// A bit per particle per row. It is a mask rather than a list because a mote
// spans at most two rows, so the whole swarm sets at most 28 bits and a row
// almost always reads zero and moves on; the loop then visits hits only. This
// is also the sort by y that was asked for and refused, in the form the refusal
// did not object to: the objection was that a cursor carries state between
// rows and garden_row takes a const GardenFrame *. An index written in prepare
// and only read in the row carries nothing.
//
// Costs 270 bytes in the scene block. Set GARDEN_MOTE_INDEX to 0 to get the
// scan back and subtract the difference on the board -- which is how the sixty
// microseconds above were obtained, and the only way a number like it can be.
#ifndef GARDEN_MOTE_INDEX
#define GARDEN_MOTE_INDEX 1
#endif
// Diagnostic: draw the motes and nothing else.
//
//   idf.py -B build_moteonly -DCMAKE_C_FLAGS="-DGARDEN_MOTE_ONLY=1" build
//
// Not a colour and not a brightness. A magenta mote would have to bypass `sun`
// to be magenta at all, which means it would exercise a different write path
// from the one that ships -- it could have proved a negative and nothing else.
// This keeps the production path exactly: same coordinates, same
// garden_shade_pixel, same (glow*q)>>8, same channel weights. What it removes
// is everything the BACKGROUND writes -- the shaft's lobe, its two shoulders,
// the ambient, the dither, the channel floors -- so whatever is on screen is
// what a mote contributed and nothing else.
//
// THE ONE THING THAT MUST SURVIVE IS `q`. The mote is gated by the shaft's own
// lobe, so blanking the shaft by zeroing q would take the motes with it and the
// build would show an empty screen whether the feature works or not -- a test
// that cannot pass. `q` is computed as normal and only the terms *written* from
// it are dropped. That distinction is the entire design of this switch and is
// the first thing someone tidying up would collapse.
//
// It changes what is drawn, not what is computed: the kernel still runs, the
// noise is still generated, the row is blanked afterwards. So it is not a
// performance build and its PERF numbers mean nothing.
//
// Reading it:
//   nothing on screen        -- the touch-up does not execute on the device,
//                               and brightness and scale are moot.
//   points at known places   -- those are the motes, on the shipping path.
//                               Lay them against the full frame to settle
//                               whether anything else is being drawn too.
// It also logs positions once a second (MOTE ...) so the capture can be
// checked arithmetically rather than by eye.
#ifndef GARDEN_MOTE_ONLY
#define GARDEN_MOTE_ONLY 0
#endif
// A mote's peak addition to `sun`, before the shaft's lobe gates it: a value
// drawn from GARDEN_GLOW_BASE .. GARDEN_GLOW_BASE+31.
//
// It was 8..23 and that was too dark to see, which is not a matter of taste
// but of arithmetic. `sun` reaches red as (5*sun)>>1 and then >>3 for the
// 5-bit channel, so a mote adds glow*5/16 steps of red at best. At glow 15,
// halved again by the two-row split and attenuated by q away from the axis,
// the typical mote moved a pixel by ONE step -- and the dither moves every
// pixel in the scene by up to one step. The swarm was drawing at exactly the
// amplitude of the noise it had to be seen against, which is why a 6x
// amplified difference of two captures showed nothing but dither.
//
// 20..51 puts the centre pixel of a well-placed mote at six to ten steps of
// red, three to five times the dither, while one at the shaft's edge is still
// gated down to nothing by q -- which is the behaviour that was wanted.
// tools/test_garden.c asserts the distribution rather than the constant.
// The pixel kernel's constant walk, fused into the arithmetic.
//
//   idf.py -B build_nofuse -DCMAKE_C_FLAGS="-DGARDEN_PIE_FUSE=0" build
//
// On by default. Twenty-one loads that occupied their own issue slot now ride
// a .LD.INCP form, which docs/pie-simd.md 3.4 measures as free: 135
// instructions a block become 114. Both spellings come from one source, so the
// only difference between the two builds is those twenty-one slots -- which is
// the point of having the switch rather than two kernels, and the reason this
// can be priced without comparing across builds.
//
// It must be bit-exact. Fusion changes when a constant is fetched, never what
// it is, so tools/pie/test_kernels.py must give byte-identical output either
// way; anything else means the transformation is not the one described here.
// The swarm, removed entirely: no motion, no index, no touch-up.
//
//   idf.py -B build_nomotes -DCMAKE_C_FLAGS="-DGARDEN_NO_MOTES=1" build
//
// This is the only way the feature can be priced. Everything asked about the
// swarm's cost so far has been answered by comparing two builds, and the answer
// -- 1.0 ms -- turned out to sit inside the 15% that i-cache alignment moves
// the same kernel between builds. One tree, one define, everything else
// identical is the measurement; two builds is an anecdote.
//
// It should have existed before the index that was built to make it faster.
//
// tools/test_garden.c's mote contracts do not hold in this build, by design --
// the swarm is what has been removed. Run it on the default build.
#ifndef GARDEN_NO_MOTES
#define GARDEN_NO_MOTES 0
#endif
// Diagnostic: draw the motes in magenta, bypassing `sun` entirely.
//
//   idf.py -B build_magenta -DCMAKE_C_FLAGS="-DGARDEN_MOTE_MAGENTA=1" build
//
// Nothing in this scene is magenta -- warm light in cool mist, green and white
// plants -- so a magenta pixel cannot be confused with a bell, a highlight, the
// shaft or the dither. It answers "does the touch-up run, and where does it
// land" without anyone having to judge whether a blob is a blob.
//
// WHAT IT DOES NOT PROVE. A magenta mote has to bypass `sun` to be magenta at
// all, so it exercises the coordinates and the write, and NOT the production
// path -- not (glow*q)>>8, not the channel weights, not the gate by the shaft.
// Magenta appearing says the code reaches those pixels. It says nothing about
// whether the shipping arithmetic is bright enough or even non-zero. Read it as
// a negative test that happens to also give you positions: no magenta means the
// touch-up does not execute and everything else is moot; magenta means look at
// GARDEN_MOTE_ONLY next, which keeps the production path and blanks the rest.
//
// Composable with GARDEN_MOTE_ONLY, which gives magenta points on black. Like
// that switch it changes what is drawn, so tools/test_garden.c's pixel
// contracts do not apply here; run them on the default build.
#ifndef GARDEN_MOTE_MAGENTA
#define GARDEN_MOTE_MAGENTA 0
#endif
#ifndef GARDEN_PIE_FUSE
#define GARDEN_PIE_FUSE 1
#endif
#ifndef GARDEN_GLOW_BASE
#define GARDEN_GLOW_BASE 20
#endif
#define GARDEN_GLOW_MAX (GARDEN_GLOW_BASE+31)
// Parameters only. Stored in the existing releasable flower scene block.
//
// MUST be zeroed before its first garden_prepare. The swarm carries state now,
// and a zeroed particle is simply one outside the shaft, which the cull replaces
// on the first frame -- so zero is a valid starting position and garbage is not.
// scene_mem zeroes a fresh block; a GardenFrame on the stack has to say so.
typedef struct {
    int sun,phase,breath;
    unsigned seed;
    GardenMote mote[GARDEN_MOTES];
#if GARDEN_MOTE_INDEX
    uint16_t rowmask[GARDEN_ROWS];  /* bit i: mote i draws on this row */
#endif
} GardenFrame;
void garden_prepare(GardenFrame *frame,float time);
void garden_row(uint16_t *row,int y,const GardenFrame *frame);
#ifdef ESP_PLATFORM
// TEMPORARY, and it goes with flower.c's counters. garden_row is now made of
// two very different things -- three vector passes over the 240 pixels, then
// trunks, canopy and grass still scalar -- and flower.c's SPLIT line cannot
// tell them apart, which is exactly the number needed to decide what to do
// next. Reads the cycles spent in the vector half and clears the count.
uint32_t garden_prof_pixels(void);
#endif
