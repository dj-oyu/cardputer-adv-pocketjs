#include "flower_parts.h"
#include "scene_mem.h"
#include "garden.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

// TEMPORARY (goes with the PIE kernel). The previous version of this counter
// timed ray_row directly and did not track: it read flat while kernel= rose
// 20 ms for work added inside the very call it wrapped. So ray is no longer
// counted, it is subtracted. total_ wraps the whole row body and garden_ wraps
// garden_row, both around calls the compiler cannot see through; work added
// anywhere in ray_row must move total_ while leaving garden_ alone, and there
// is no window it can hide in.
//
// The instrument checks itself against the only figure that has never
// contradicted itself: total should equal kernel= less glass_rain and the strip
// scaffolding, a couple of ms. If it does not, believe kernel= and delete this.
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#include "esp_log.h"
#define PROF_FENCE __asm__ __volatile__("":::"memory")
static uint32_t prof_total,prof_garden,prof_visits,prof_hits,prof_frames;
static uint32_t prof_sqrt,prof_sqrtn,prof_shade,prof_bell,prof_belln;
static uint32_t prof_span,prof_spann,prof_div,prof_divn,prof_scan,prof_pre;
// prep. It is 3.2 ms -- larger than everything the swarm thread argued about
// put together -- and it has never been split. Four counters rather than four
// builds, because the last two attributions in this file were both artefacts
// of subtracting one build from another.
//
// flower_build_botanicals lives in flower_species.c, which belongs to the
// board's owner. Timing a call is not reading the callee, so this measures it
// from outside and says nothing about what is inside it.
static uint32_t prof_pgarden,prof_pseeds,prof_pbuild,prof_ppetal,prof_ppetaln;
// The filter's own cost, reported like every other expensive thing in the SPLIT
// line -- cycles and the count they are spread over. It had none, so its 0.4 ms
// was an estimate sitting among seven measurements; and the pixel counts it was
// estimated from (1590 / 4828 / 8369 a frame at one, two and three stages) are
// now a prediction this can check.
static uint32_t prof_horror,prof_horrorn;
#endif

// Orthographic primary rays intersect thin ellipsoids analytically. This is
// actual visibility tracing, but the pearl/glass lighting is an approximation:
// no secondary rays, refraction, or physically based transparency is claimed.
#define W 240
#define H 135
#define X0 120
// The dither on shade's rounding. On by default: it is what stops the plant
// reading as a cut-out.
#ifndef FLOWER_DITHER
#define FLOWER_DITHER 1
#endif
// Where the grain starts, and how dense it gets at the bottom edge (of 255).
// The grained band is a fraction of the PLANT, not a strip of glass. A screen
// row is a different fraction of every species -- the same reason aim is a
// fraction of the plant's height and a constant zoom cropped thirteen species
// of fourteen. SPAN is measured up from the plant's lowest point, so the same
// fifth is grained whether the camera is fitted or walked in.
#ifndef FLOWER_GRAIN_SPAN
#define FLOWER_GRAIN_SPAN 0.40f
#endif
// Three levels and not a ramp, as fractions of the span measured up from the
// plant's foot. The two lower ones keep the ABSOLUTE size they had when the
// span was 0.20 and the owner approved them -- 0.09 and 0.11 of the plant --
// so widening the band adds a level on top rather than restretching the two
// that were already right.
// Unequal on purpose: widths 0.18 / 0.26 / 0.56 of the span, so the lightest
// level is the widest and the heaviest the narrowest. Real dirt and vignetting
// fall off that way -- a long faint approach and a short dense end -- and equal
// thirds read as three painted stripes because each is the same size.
#ifndef FLOWER_GRAIN_SPLIT
#define FLOWER_GRAIN_SPLIT 0.18f
#endif
#ifndef FLOWER_GRAIN_SPLIT2
#define FLOWER_GRAIN_SPLIT2 0.44f
#endif
// THE SEAMS ARE VISIBLE, and that is not a failure of the levels. A horizontal
// line across the plant is what an eye finds even when both sides of it are
// noise -- the eye is looking for the edge, not for the texture. So the seam is
// broken two ways at once, and they do different jobs.
//
// RAG is spatial and still: a per-column offset that makes the boundary a torn
// edge instead of a line. This is the one that actually removes the artefact,
// because a line that is not straight is not a line.
//
// JITTER is temporal and slow: every N frames each boundary moves a row or two.
// SLOW is the whole of it. Moving a boundary every frame does not hide it, it
// turns a static line into a crawling one, which is more visible than what it
// was hiding. N frames of stillness is what keeps it below notice.
#ifndef FLOWER_GRAIN_RAG
#define FLOWER_GRAIN_RAG 2
#endif
#ifndef FLOWER_GRAIN_JITTER
#define FLOWER_GRAIN_JITTER 11
#endif
#ifndef FLOWER_GRAIN_MID
#define FLOWER_GRAIN_MID 84
#endif
// The seam against the clean plant is now the one a viewer looks at, so the
// step across it is the smallest of the three: 33% -> 12% -> 0 rather than the
// 33% -> 0 it used to be.
#ifndef FLOWER_GRAIN_LOW
#define FLOWER_GRAIN_LOW 30
#endif
#ifndef FLOWER_GRAIN_MAX
#define FLOWER_GRAIN_MAX 215
#endif
// How far a grain pixel moves, in quantisation steps of the six-bit channel.
// Density says how many pixels; this says how far each goes, and they are
// independent -- the density was right while this was a quarter of what it
// needed to be. 4 puts a grain pixel three to four steps either side of its
// value, against a leaf that sits around 22 of 63.
#ifndef FLOWER_GRAIN_AMP
#define FLOWER_GRAIN_AMP 4
#endif
// How far the window is widened past the outermost petal, so the softening
// reaches the outline instead of stopping at it.
// The horror event: the early-HDR blowout and a negative, together, over the
// whole frame. OFF BY DEFAULT and it should stay that way until somebody has
// seen it -- FLOWER is the settings background, and this is the one effect in
// this file a person could meet without asking for it.
//
// Built as an EVENT and not a mode. A permanently inverted settings screen is
// not usable; two hundred milliseconds of one now and then is. That also
// removes the budget argument rather than answering it: an effect on two
// percent of frames costs two percent of its per-frame price, so the per-frame
// price stops being the number that decides anything. If it turns out to be
// wanted continuously, a trigger that always fires is a steady state, and
// nothing here prevents that.
//
// Deliberately overdone. The blowout is the notorious kind -- no threshold, the
// whole frame blooming, halos everywhere -- because the overdone quality is the
// content here and not a defect to tune out.
#ifndef FLOWER_HORROR
#define FLOWER_HORROR 0
#endif
// Seconds between events on average, and how long one lasts.
#ifndef FLOWER_HORROR_EVERY
#define FLOWER_HORROR_EVERY 37.0f
#endif
#ifndef FLOWER_HORROR_MS
#define FLOWER_HORROR_MS 220
#endif
// The halo outside the silhouette. Radius in pixels, each side of the row.
#ifndef FLOWER_BLOOM
#define FLOWER_BLOOM 1
#endif
#ifndef FLOWER_BLOOM_R
#define FLOWER_BLOOM_R 5
#endif
// How bright a petal pixel has to be to seed a halo, in sixths of the green
// channel's 0..63. Below it, nothing is emitted at all.
#ifndef FLOWER_BLOOM_MIN
#define FLOWER_BLOOM_MIN 34
#endif
#ifdef ESP_PLATFORM
#endif
// The camera, set by the rotator and read by flower_prepare. Preparing a
// species directly -- which the tests do -- leaves them at the old fixed pose,
// so that path is unchanged and its assertions still mean what they meant.
// Consumed by the next flower_prepare and then forgotten. Naming a species
// directly -- which the tests do, and which any future caller would -- must not
// pick up whichever shot the rotator happened to leave framed; that is scene
// state leaking into a function whose whole contract is "draw this species".
static float view_yaw=0,view_pitch=.12f;
static int view_pending;
#define FW 120
#define LAT FLOWER_BELL_BANDS
#define SCALE 37.0f
// Pixels of clearance a fitted shot leaves on each edge, so the plant sits in
// the frame rather than touching it.
// Six is enough again. It was raised to ten while the real defect was the
// vertical centring below, and a symmetric margin cannot fix an asymmetric
// offset -- it only shrank the plant and wasted more room at the bottom. With
// the centring right, six passes on every species, which is the margin doing
// its own job: absorbing the little the rendered silhouette runs past the
// world half-extents (a bell's lip flares, and the sway moves the plant after
// the frame is chosen).
#ifndef FLOWER_FIT_MARGIN
#define FLOWER_FIT_MARGIN 6
#endif
// The three arrays below live in the shared scene block, not in .bss: see
// scene_mem.h. Declaring them as pointers rather than arrays is what keeps
// every use site unchanged -- petals[i] reads the same either way, here and in
// the builders next door.
Petal *petals;
static uint8_t *seed_map;
static float *depth;
#define FLOWER_BYTES (sizeof(Petal)*MAX_PARTS+sizeof(float)*FW+32*32+sizeof(GardenFrame))
// Any address unique to this file identifies it to the block.
static const char flower_owner;
unsigned count;
static flower_species_t current_species;
static bool seeds_ready;
static float bell_slopes[LAT],bell_offsets[LAT],bell_lo[LAT],bell_hi[LAT],bell_rmax2;
static void prepare_seeds(void);
// There is no vertex mesh. There was one -- 8 petals x 7 x 13 vertices, 17,472
// bytes of .bss -- feeding a triangle rasteriser that only background mode 4
// ever used, and it was the second largest single object in the firmware, held
// from boot to power-off whether or not anybody was looking at a flower. On a
// board with no PSRAM that is 17 KB the heap never gets, and the heap is where
// the JS guest, the font atlas and the Wi-Fi driver all come from: the radio
// needs about 48 KB free and an app leaves it 9. Every mode now takes the
// analytic path, which stores no geometry at all -- an ellipsoid is six
// coefficients and a ray meets it in closed form. The triangles were the only
// reason to keep vertices.
float elapsed;

// ---------------------------------------------------------------------------
// The rotation behind the single FLOWER menu row.
//
// One row draws the botanical collection in turn.
//
// Changing species makes flower_build_botanicals() rebuild the whole part list -- 31 parts
// for the lily, 36 for the sunflower, 20 for the snowdrop -- so the plant is
// replaced between one frame and the next. Cutting straight from one to the
// other reads as a glitch, so the swap happens behind a dissolve: the shading
// is scaled down to the sky already in the strip, the species changes at the
// frame where that factor reaches exactly zero, and it comes back up. Nothing
// is stored for it -- this is a per-row strip renderer and the factor is a
// multiply on the way out, so the whole dissolve costs one blend per lit pixel.
// ---------------------------------------------------------------------------

// Long enough that the flower is a background rather than a slideshow, short
// enough that somebody looking at the home screen sees the plant change.
// The shots, and they are cut rather than panned.
//
// The first version swung the camera a quarter turn between evenly spaced
// angles, smoothstepped over two and a half seconds. Two things were wrong with
// it. Evenly spaced angles are not viewpoints -- they are a turntable, and a
// turntable has no opinion about what it is looking at. And a translation or
// rotation animated inside a still scene reads as cheap unless it is very
// carefully motivated; the plant already sways, and a camera sliding over the
// top of that sway looked like a screensaver.
//
// So there is no camera movement at all. Each shot is held, then the plant
// dissolves and the next shot is already framed -- a cut, which is what a
// photographer's sequence of a subject actually is. The dissolve exists for the
// species change and now carries both, so a view change and a plant change are
// the same event and there is one mechanism rather than two.
//
// The angles are chosen the way a plant is photographed rather than by dividing
// a circle:
//
//   three-quarter    the portrait angle -- both the face of the flower and the
//                    depth behind it, which straight-on flattens away
//   profile          the stem's line and the bell's silhouette, level
//   rim              from behind the shoulder, so the shaft's light comes
//                    through the petals rather than onto them
//   low, looking up  how a hanging flower has to be shot to see inside it,
//                    which for the valley and the fritillary is the only angle
//                    that shows what the plant is
// The shot list lives in flower_shots.h, with the renderer's constraints
// written down beside it: what pitch means in an orthographic projection, why
// there is no yaw, why aim is a fraction rather than a row. Composing the
// sequence is a different skill from making it draw, and the seam is here.
#include "flower_shots.h"
// The camera, derived from the shot and the plant it is pointed at. Set once a
// frame; the six places the projection appears all read it, so there is one
// definition of where the camera is rather than a constant in each.
static float cam_s=SCALE,cam_inv=1.0f/SCALE,cam_x=0,cam_y=0;
// The grained band, projected once a frame. Rows grow downward: grain_top is
// the top of the whole band, and the levels get heavier going down past
// grain_b1 and then grain_b2.
static int grain_top=H,grain_b1=H,grain_b2=H;
// One jitter per boundary and INDEPENDENT: a shared offset slides all three
// together, and three boundaries moving in lockstep read as the whole band
// breathing, which is a bigger thing to notice than the seams were.
static int grain_jit[3];
// The seam for one column of one boundary. A row decision, not a pixel one --
// nothing here depends on y, so the pixel loop is unchanged in shape.
static int grain_seam(int x,int b) {
    // x>>1 so the tear comes in runs of two columns. Per-column white noise at
    // this amplitude reads as a blurred boundary rather than a torn one.
    unsigned h=(unsigned)(x>>1)*2654435761u+(unsigned)b*0x9E3779B9u;
    return grain_jit[b]+(int)((h>>11)%(2u*FLOWER_GRAIN_RAG+1u))-FLOWER_GRAIN_RAG;
}
#define FLOWER_ROTATE_S 40.0f
// Total dissolve, half of it either side of the swap: about 18 frames down and
// 18 back up at 30 fps. Long enough to read as a dissolve rather than a blink,
// short enough not to look like a fault.
#define FLOWER_FADE_S    1.2f
#define FLOWER_FADE_HALF (FLOWER_FADE_S*0.5f)

// 0 = the sky alone, 1 = shade() exactly as it stands. The endpoints are exact
// rather than approached, so a frame at 1 is bit-identical to one drawn with no
// rotation at all -- which is what lets the per-species tests keep working.
static float bloom_fade=1;
static float bloom_elapsed;              // seconds the current SHOT has been up
static int bloom_view;                   // which of FLOWER_SHOTS is framed
static flower_species_t bloom_species=FLOWER_VALLEY;

// xorshift32, seeded by a constant. Deterministic on purpose: the host test
// drives the same sequence the device does, which is the only way to assert
// that the botanical collection comes up and every swap is hidden. A per-boot seed
// would buy unpredictability nobody asked for and cost the test its evidence.
static uint32_t bloom_rng=0x9e3779b9u;
static uint32_t bloom_garden_seed,bloom_garden_old_seed;
static unsigned bloom_garden_mix=256;
#define FLOWER_GARDEN_FADE_S 3.0f
static uint32_t bloom_random(void) {
    bloom_rng^=bloom_rng<<13; bloom_rng^=bloom_rng>>17; bloom_rng^=bloom_rng<<5;
    return bloom_rng;
}
// One of the other botanicals, so a draw never repeats the plant already up:
// a rotation that shows the same flower twice looks like it has stopped.
static flower_species_t bloom_next(flower_species_t from) {
    unsigned n=FLOWER_SPECIES_COUNT-FLOWER_VALLEY;
    unsigned here=(unsigned)from-FLOWER_VALLEY;
    unsigned step=1+bloom_random()%(n-1);
    return (flower_species_t)(FLOWER_VALLEY+(here+step)%n);
}
// Every float `/` in this file used to be a call. The FPU on this part has no
// divide instruction, GCC emits no seed sequence for one, and -mlongcalls turns
// the call into `l32r` + `callx8` into a ROM address -- so it is invisible to a
// mnemonic search and to a `call8 <symbol>` search alike, which is why two
// rounds of planning here were built on "ray_row has no divisions". It has
// sixteen. See docs/pie-simd.md 3.7.
//
// A call is worse than its own cycles. `__divsf3` and `fmaxf` take their
// arguments in *integer* registers, so each one costs an `rfr`/`wfr` pair and
// spills whatever floats are live around it: ray_row carries 97 float
// arithmetic instructions against 158 float loads and stores, and shade has
// more register-file moves than arithmetic. Removing a divide removes the
// traffic around it, not just the routine.
//
// FLOWER_DIV_EXACT restores the original expressions so tools/test_flower_div.c
// can hold the two against each other pixel for pixel; without it, the hot
// paths multiply by a reciprocal computed once per part per frame.
// Each optimisation in this file has a switch that turns it off, so that its
// effect on the device can be measured by flashing both and subtracting. That
// is a requirement rather than a nicety, and it is here because of a specific
// mistake: bell_reject shipped in the same commit as the counter that was meant
// to measure it, so its device effect is unknown and cannot now be recovered.
// The host numbers stand on their own -- 40.8% of bell visits rejected, zero
// hits dropped -- but "40.8% of visits" is not "40.8% of the time", which is
// exactly the distinction that retired tools/flower-rejection.
//
//   FLOWER_DIV_EXACT        the divisions, instead of reciprocal multiplies
//   FLOWER_NO_BAND_HOIST    the band bounds recomputed inside bell_hit
//   FLOWER_NO_SPAN_AFFINE   the three dot products, instead of dx*A+B
//   FLOWER_NO_BELL_REJECT   no early rejection; walk all six bands always
//   FLOWER_BELL_CHECK       compute the rejection, do not act on it, and count
//                           the visits where it was wrong (must be zero)
#ifdef FLOWER_DIV_EXACT
#define DIVR(num,inv,den) ((num)/(den))
#define POS(x)            fmaxf(0,(x))
#else
#define DIVR(num,inv,den) ((num)*(inv))
#define POS(x)            ((x)>0?(x):0)
#endif
#define INV_SCALE (1.0f/SCALE)
// A Petal is not finished when its radii are set: bell_hit and shade read
// inv_radius on their hot paths, and a Petal that has radii but no reciprocals
// draws from uninitialised memory rather than failing. So the derivation is a
// named call rather than three lines inside one loop -- there is one thing to
// remember, it is greppable, and tools/test_flower.c calls the same one when it
// builds a Petal by hand for the analytic checks.
static void petal_reciprocals(Petal *p) {
    for(int j=0;j<3;j++)p->inv_radius[j]=1/p->radius[j];
    // The ray direction in the petal's own scaled frame. It depends on the
    // part and not on the pixel, so bell_hit was recomputing it on every one
    // of its ~2,000 visits a frame; and having it here is what lets the two
    // reciprocals below exist at all, which is what keeps bell_reject free of
    // division.
    for(int j=0;j<3;j++)p->bd[j]=p->axis[j].z*p->inv_radius[j];
    // o[j] is affine in x, and this is its slope. The span walks x by one, so
    // the three dot products bell_hit was doing per visit -- nine multiplies
    // and six adds -- become dx*oax[j] + (the row's constant term): three and
    // three. Written as a re-evaluation from dx rather than a running sum on
    // purpose: a forward difference over a 120-pixel span accumulates about
    // 7e-6 relative, and o[] feeds the discriminant whose *sign* decides
    // whether a pixel is inside the silhouette. Re-evaluating costs one extra
    // multiply and cannot drift.
    for(int j=0;j<3;j++)p->oax[j]=p->axis[j].x*p->inv_radius[j];
    p->ba0=p->bd[0]*p->bd[0]+p->bd[2]*p->bd[2];
    p->inv_a0=p->ba0>0?1/p->ba0:0;
    p->inv_d1=p->bd[1]!=0?1/p->bd[1]:0;
}
static int clampi(int x,int lo,int hi) { return x<lo?lo:x>hi?hi:x; }
static uint16_t rgb(int r,int g,int b) {
    return (uint16_t)((clampi(r,0,255)>>3)<<11 |
                      (clampi(g,0,255)>>2)<<5 | (clampi(b,0,255)>>3));
}
// The menu row's entry point. Advances the rotation, then prepares whatever
// species the rotation currently holds.
#if FLOWER_HORROR
static void horror_tick(float dt);
#endif
// Advanced once a frame so the grain moves. Not static, and that is the point:
// the plant is no longer a pure function of (species, elapsed), which is a
// deliberate consequence of grain that moves and an accidental break of every
// test that compares two renders of the same moment. Pinning it is how a test
// says "the same frame", the way hush_swarm() says "the same swarm".
unsigned flower_grain_frame;
#define grain_frame flower_grain_frame
void flower_prepare_rotating(float dt,int tilt_x,int tilt_y) {
    if(!isfinite(dt)||dt<0)dt=0;
    dt=fminf(dt,.1f);
    bloom_elapsed+=dt;
    grain_frame++;                       /* the grain moves with the frame */
#if FLOWER_HORROR
    horror_tick(dt);
#endif
    float fade;
    if(bloom_elapsed>=FLOWER_SHOTS[bloom_view].hold-FLOWER_FADE_HALF) {
        fade=(FLOWER_SHOTS[bloom_view].hold-bloom_elapsed)/FLOWER_FADE_HALF;
        if(fade<=0) {
            // The darkest frame. Nothing of the old shot is on the strip, so
            // this is the one frame where the camera may be moved and the part
            // list replaced -- and it is the only frame either happens on.
            // Cutting here is why there is no pan: the change is instant and
            // invisible, which a slide across two and a half seconds is not.
            fade=0;
            if(++bloom_view>=FLOWER_VIEWS) {
                bloom_view=0;
                bloom_species=bloom_next(bloom_species);
                bloom_garden_old_seed=bloom_garden_seed;
                bloom_garden_seed=bloom_rng;
            }
            bloom_elapsed=0;
        }
    }
    else if(bloom_elapsed<FLOWER_FADE_HALF) fade=bloom_elapsed/FLOWER_FADE_HALF;
    else fade=1;
    // Which of the N viewpoints, and how far through the swing to the next.
    //
    // The angles are derived from the index rather than stored: a whole turn
    // divided by N, so the plant is walked around and comes back to where it
    // started as the species changes. Pitch nods on a different divisor so the
    // two do not repeat together and the four views are four genuinely
    // different looks rather than one look rotated.
    view_yaw=0;
    view_pitch=FLOWER_SHOTS[bloom_view].pitch;
    // The cap, enforced rather than trusted: the table is edited by eye and a
    // number past this stops reading as a camera and starts reading as a
    // toppled plant.
    if(view_pitch>FLOWER_PITCH_MAX)view_pitch=FLOWER_PITCH_MAX;
    if(view_pitch<-FLOWER_PITCH_MAX)view_pitch=-FLOWER_PITCH_MAX;
    view_pending=1;
    // After flower_prepare, which resets the dissolve: preparing one species
    // directly is what the tests and any future caller do, and that path has no
    // rotation to hide.
    float forest=fminf(1,bloom_elapsed/FLOWER_GARDEN_FADE_S);
    bloom_garden_mix=(unsigned)(forest*forest*(3-2*forest)*256);
    flower_prepare(dt,tilt_x,tilt_y,bloom_species);
    // Reconfigure the forest every flower interval, but never expose its cut.
    // Only vegetation dissolves; atmosphere and swarm continue uninterrupted.
    fade=fmaxf(0,fminf(1,fade));
    bloom_fade=fade*fade*(3-2*fade);
}
flower_species_t flower_current_species(void) { return bloom_species; }
float flower_fade(void) { return bloom_fade; }

void flower_prepare(float dt,int tilt_x,int tilt_y,flower_species_t species) {
    // A return from an app must not advance the flower by minutes in one frame.
    if(!isfinite(dt)||dt<0)dt=0;
    dt=fminf(dt,.1f);elapsed=fmodf(elapsed+dt,120*PI);
    // The board's tilt used to reach the orientation from here, through a
    // first-order filter into tx/ty. It is gone, and the parameters are kept
    // only because SCENES[]'s row shape requires this signature.
    //
    // It was not removed for the cycles -- it was two multiply-adds a frame.
    // It was removed because it did not read on the glass: a board sitting on
    // a desk reports TILT 6 0, so tx settled at about 0.012 rad and stayed
    // there. A coupling that only moves when someone deliberately waves the
    // machine is paying, in every frame and in every test, for something
    // nobody sees.
    //
    // What it buys back is larger than what it cost. The flower is now a pure
    // function of `elapsed`, so two frames at the same time are the same
    // frame, and an approximation to it -- a cached trace reused for a few
    // frames, say -- can be compared against the exact thing for the same
    // moment with nothing external to hold still. tools/flower-stale.c is that
    // comparison, and it could not have been written while tilt was wired.
    (void)tilt_x;(void)tilt_y;
    current_species=species>=0&&species<FLOWER_SPECIES_COUNT?species:FLOWER_VALLEY;
    // Naming a species outright means drawing it, not dissolving into it. The
    // rotation sets its own factor after this returns.
    bloom_fade=1;
    if(!view_pending)bloom_garden_mix=256; // direct rendering has no transition
    bool rebuild;
    char *m=scene_mem(&flower_owner,FLOWER_BYTES,&rebuild);
    if(!m) { count=0;petals=NULL;depth=NULL;seed_map=NULL;return; }
    petals=(Petal*)m;
    depth=(float*)(m+sizeof(Petal)*MAX_PARTS);
    seed_map=(uint8_t*)(depth+FW);
#ifdef ESP_PLATFORM
    PROF_FENCE;uint32_t q0=esp_cpu_get_cycle_count();PROF_FENCE;
#endif
    if(view_pending)
        garden_prepare_layout((GardenFrame*)(seed_map+32*32),elapsed,
            bloom_garden_old_seed,bloom_garden_seed,bloom_garden_mix);
    else garden_prepare((GardenFrame*)(seed_map+32*32),elapsed);
    // The spiral and the bell profile are cached in that block, so they are as
    // new as it is. Keeping the flag outside and the data inside would be the
    // one way to get this wrong.
    if(rebuild)seeds_ready=false;
#ifdef ESP_PLATFORM
    PROF_FENCE;uint32_t q1=esp_cpu_get_cycle_count();PROF_FENCE;
#endif
    prepare_seeds();
#ifdef ESP_PLATFORM
    PROF_FENCE;uint32_t q2=esp_cpu_get_cycle_count();PROF_FENCE;
#endif
    // Plants sway around their roots rather than rotating upside down, and the
    // sway is now a small motion ON TOP of wherever the camera is standing.
    // Copied, not consumed. The flag has TWO readers -- the angles here and the
    // framing two hundred lines below -- and clearing it at the first meant the
    // second always saw zero, took the unframed branch, and threw the shot's
    // zoom and aim away. Only pitch survived, because it is read before the
    // clear, so every camera change made today played as four pitches of one
    // wide shot. The framing, the pull-back from 2.60 to 1.80, the whole size
    // ladder: none of it ever reached the glass.
    //
    // The flag is cleared at the end of the function now, where "consumed by
    // this prepare" is actually true. A single-reader flag that grew a second
    // reader is worth watching for elsewhere in this file.
    const int framed=view_pending;
    // Fine Nigella bracts magnify the common rocking motion on screen.
    // Slow the whole flower together so the lace stays attached and calm.
    float rocking=current_species==FLOWER_NIGELLA
        ?.012f*sinf(elapsed*.1f):current_species==FLOWER_ECHINACEA
        ?0:.035f*sinf(elapsed*.6f);
    float yaw=(framed?view_yaw:0)+rocking;
    float pitch=framed?view_pitch:.12f;
    flower_build_botanicals(current_species,yaw,pitch);
#ifdef ESP_PLATFORM
    PROF_FENCE;uint32_t q3=esp_cpu_get_cycle_count();PROF_FENCE;
    prof_pgarden+=q1-q0;prof_pseeds+=q2-q1;prof_pbuild+=q3-q2;
#endif
    for(unsigned i=0;i<count;i++) {
        Petal *p=&petals[i];
        for(int j=0;j<6;j++)p->q[j]=0;
        float ex=0,ey=0;
        petal_reciprocals(p);
        for(int j=0;j<3;j++) {
            V b=p->axis[j];float r=p->radius[j],ir=p->inv_radius[j];
            float k=ir*ir;
            p->q[0]+=b.x*b.x*k;p->q[1]+=b.y*b.y*k;p->q[2]+=b.z*b.z*k;
            p->q[3]+=b.x*b.y*k;p->q[4]+=b.x*b.z*k;p->q[5]+=b.y*b.z*k;
            ex+=b.x*b.x*r*r;ey+=b.y*b.y*r*r;
        }
        p->invzz=1/p->q[2];ex=sqrtf(ex);ey=sqrtf(ey);
        // A bell fits a box, not an ellipsoid, and its lip flares past r=1.
        if(p->shape) {
            ex=ey=0;
            for(int j=0;j<3;j++) {ex+=fabsf(p->axis[j].x)*p->radius[j]*1.12f;ey+=fabsf(p->axis[j].y)*p->radius[j]*1.12f;}
        }
        // World extent kept, screen boxes deferred: the camera is not known
        // until the plant it is framing has been measured, and the boxes are in
        // screen space. In Petal at the owner's direction -- it was in a local
        // array here first, to keep out of their header while they were editing
        // it, which cost 448 bytes of stack for the same two numbers.
        p->ex=ex;p->ey=ey;
    }
    // The frame. A shot names a fraction of the plant's height and a zoom, so
    // the same table gives an establishing shot of a tall iris and a close-up
    // of a short hyacinth without either being written down anywhere.
    {
        float lo=1e30f,hi=-1e30f,xs=0,xlo=1e30f,xhi=-1e30f;
        for(unsigned i=0;i<count;i++) {
            if(petals[i].c.y-petals[i].ey<lo)lo=petals[i].c.y-petals[i].ey;
            if(petals[i].c.y+petals[i].ey>hi)hi=petals[i].c.y+petals[i].ey;
            if(petals[i].c.x-petals[i].ex<xlo)xlo=petals[i].c.x-petals[i].ex;
            if(petals[i].c.x+petals[i].ex>xhi)xhi=petals[i].c.x+petals[i].ex;
            xs+=petals[i].c.x;
        }
        if(!count||hi<=lo) { lo=-1.4f;hi=1.0f; }
        if(!framed) {
            // Naming a species directly is the unframed path: the projection
            // it had before any of this existed, so every assertion written
            // against it still means the same thing. Re-centring here moved
            // the plant off the bottom of the screen and the "every species is
            // rooted at the edge" check caught it on the first species.
            cam_s=SCALE;cam_inv=1.0f/SCALE;cam_x=0;cam_y=0;
        } else {
            // zoom <= 0 asks for "whatever fits". A constant cannot frame
            // every species: they differ in size by more than the shots differ
            // from each other, which is the same reason aim is a fraction of
            // the plant's height rather than a row. At a fixed 1.00 the
            // establishing shot was cutting parts off the widest plants -- the
            // shot whose whole job is to show the whole plant.
            //
            // The window is 120 px wide and 123 rows tall, and the margin
            // leaves the plant off the edges rather than touching them.
            float zoom=FLOWER_SHOTS[bloom_view].zoom;
            if(zoom<=0) {
                float wx=xhi-xlo,wy=hi-lo;
                float fx=wx>0.001f?(FW-2*FLOWER_FIT_MARGIN)/(wx*SCALE):99.0f;
                // 123 rows is the window, and the whole of it is usable now
                // that the plant is centred in it rather than on row 65.
                float fy=wy>0.001f?(123-2*FLOWER_FIT_MARGIN)/(wy*SCALE):99.0f;
                zoom=fx<fy?fx:fy;
                if(zoom>1.0f)zoom=1.0f;      /* never magnify to fit */
            }
            cam_s=SCALE*zoom;
            cam_inv=1.0f/cam_s;
            cam_x=count?xs/(float)count:0;
            // Fitting frames the whole plant, so its centre is the middle of
            // the plant and not the shot's aim.
            // Centred on the WINDOW, not on the projection's origin. The
            // projection puts cam_y at screen row 65, but the window is rows
            // 12..134 whose middle is row 73 -- so centring the plant on cam_y
            // gave it 53 rows above and 69 below, overflowed the top by about
            // eight and wasted fifteen at the bottom. Twelve of fourteen
            // species sat exactly on row 12.
            //
            // Shifting cam_y by the eight rows between the two centres uses the
            // whole window; fitting against 2*(65-12) rows instead would also
            // stop the crop but would keep throwing the bottom third of the
            // headroom away. A bigger margin fixes neither: it is symmetric, so
            // it only shrinks the plant and wastes more below.
            cam_y=FLOWER_SHOTS[bloom_view].zoom<=0
                 ?(lo+hi)*0.5f+(73.0f-65.0f)/cam_s
                 :lo+(hi-lo)*FLOWER_SHOTS[bloom_view].aim;
        }
        // Plant space to screen space once, here, where lo/hi and the camera
        // are both known -- rather than a row constant that would mean a
        // different fraction of every species and of every zoom.
        {
            float span=(hi-lo)*FLOWER_GRAIN_SPAN;
            grain_top=clampi((int)(65.0f-cam_s*(lo+span-cam_y)),12,H);
            grain_b1=clampi((int)(65.0f-cam_s*(lo+span*FLOWER_GRAIN_SPLIT2-cam_y)),12,H);
            grain_b2=clampi((int)(65.0f-cam_s*(lo+span*FLOWER_GRAIN_SPLIT-cam_y)),12,H);
            if(grain_b1<grain_top)grain_b1=grain_top;
            if(grain_b2<grain_b1)grain_b2=grain_b1;
        }
    }
    for(unsigned i=0;i<count;i++) {
        Petal *p=&petals[i];
        p->xmin=clampi((int)floorf(180+cam_s*(p->c.x-p->ex-cam_x)),X0,W-1);
        p->xmax=clampi((int)ceilf(180+cam_s*(p->c.x+p->ex-cam_x)),X0,W-1);
        p->ymin=clampi((int)floorf(65-cam_s*(p->c.y+p->ey-cam_y)),12,H-1);
        p->ymax=clampi((int)ceilf(65-cam_s*(p->c.y-p->ey-cam_y)),12,H-1);
    }
#ifdef ESP_PLATFORM
    // Per part, not per frame: the species differ by a factor of three in part
    // count, so a frame figure alone cannot say whether this loop is expensive
    // or merely long.
    PROF_FENCE;prof_ppetal+=esp_cpu_get_cycle_count()-q3;prof_ppetaln+=count;PROF_FENCE;
#endif
    // Consumed here, after both readers. See the note at `framed`.
    view_pending=0;
}
static void prepare_seeds(void) {
    if(seeds_ready)return;
    // Vogel's spiral: a small procedural material cache, generated in RAM.
    // It is neither an embedded image nor a list of stored floret positions.
    for(int i=0;i<32*32;i++)seed_map[i]=0;
    for(int i=0;i<140;i++) {
        float a=i*2.39996323f,r=14.5f*sqrtf((i+.5f)/140);
        int x=(int)(16+r*cosf(a)),y=(int)(16+r*sinf(a));
        seed_map[y*32+x]=(uint8_t)(100+i%4*40);
    }
    float previous=0;
    bell_rmax2=0;
    for(int i=0;i<LAT;i++) {
        float t=(float)(i+1)/LAT,k=2*t-1;
        float radius=t<.5f?sqrtf(fmaxf(0,1-(1-2*t)*(1-2*t)))*.88f:.88f+.24f*k*k*k;
        // The band's height bounds are a function of the band index and
        // nothing else, so they belong here and not in bell_hit -- where they
        // were two `2.0f*band/LAT` divisions, and a division on this part is a
        // call into a ROM routine (docs/pie-simd.md 3.7). Six bands, twice
        // each, on every one of ~2,000 bell visits a frame.
        bell_lo[i]=-1+2.0f*i/LAT;
        bell_hi[i]=-1+2.0f*(i+1)/LAT;
        bell_slopes[i]=(radius-previous)*LAT*.5f;
        bell_offsets[i]=previous-bell_slopes[i]*bell_lo[i];
        previous=radius;
        // The widest the bell ever gets, for bell_reject. Taken over the whole
        // of each band and then over all bands, and inflated 2% so that the
        // rounding in a test built from squares and products can only ever make
        // it reject less.
        float rlo=bell_slopes[i]*bell_lo[i]+bell_offsets[i];
        float rhi=bell_slopes[i]*bell_hi[i]+bell_offsets[i];
        float m=fabsf(rlo)>fabsf(rhi)?fabsf(rlo):fabsf(rhi);
        if(m*m*1.02f>bell_rmax2)bell_rmax2=m*m*1.02f;
    }
    seeds_ready=true;
}
// Ordered dither on the rounding, not grain on top of it.
//
// The premise was checked before this was built and it came back INVERTED: the
// plant already carries four times the background's pixel-to-pixel variation
// (1.553 against 0.380). What separates the two surfaces is not that one is
// noisy and the other is not -- it is that the background's variation is
// uncorrelated dither and the plant's is STRUCTURED. A normal that varies
// smoothly across a petal, dropped into five and six bits, produces contours:
// bands that follow the geometry, which the eye reads as an edge rather than as
// a surface. That is the 一種の鮮明さ that makes the flower look pasted on.
//
// So this adds nothing. It moves the rounding decision by up to one
// quantisation step, so the contour is traded for noise the background already
// has. Amplitude is one full step per channel and not less: red and blue drop
// three bits (step 8, so 0..6 from garden_dither's 0..3 doubled) and green
// drops two (step 4, so 0..3 directly). A dither smaller than the step it is
// hiding cannot cross a band boundary and does nothing at all.
//
// garden_dither and not a second noise: it is already asserted uniform over its
// four values and uncorrelated with both neighbours, and it is the noise the
// background is already wearing. Two surfaces dithered by two different noises
// separate as surely as one dithered and one not. Ordered and not error
// diffusion -- diffusion carries a dependency to the next pixel, which makes it
// sequential and unvectorisable for nothing this needs.
static int sh_px,sh_py;                  /* the pixel being shaded */
static int sh_grain;                     /* is this surface a leaf or a stem */
static uint16_t rgbd(int r,int g,int b) {
#if FLOWER_DITHER
    // Two different jobs share one call. Every plant pixel gets its rounding
    // dithered, which is what stops the smooth normals contouring into bands.
    // Leaves and stems additionally get GRAIN: the same full-step perturbation,
    // applied to only some of the pixels, more of them toward the bottom of the
    // screen.
    //
    // DENSITY AND NOT AMPLITUDE, and that is forced rather than chosen. A
    // perturbation smaller than one quantisation step cannot cross a band
    // boundary, so it does nothing at all -- which is why the first two
    // versions of this effect were invisible. 「うっすら」 therefore cannot be
    // a quieter dither; it has to be fewer pixels, each still moved a whole
    // step. Then every perturbation that happens is one that shows, and the
    // gradient is in how many of them there are.
    //
    // The frame number is in the index. A static pattern reads as texture that
    // belongs to the surface; a moving one reads as grain that belongs to the
    // image. That is one more term in the hash and most of the difference
    // between this being noticed and not.
    // The two jobs move differently, and that separation is the effect. The
    // banding dither is a function of the pixel alone -- stable, like the
    // garden's, because a rounding fix that crawled would be a new artefact.
    // The grain is a function of the pixel AND the frame, because a static
    // pattern reads as texture belonging to the surface and a moving one reads
    // as grain belonging to the image. Making both move put the whole plant in
    // motion and lost the distinction the grain is for.
    int d=garden_dither(sh_px,sh_py);
    if(sh_grain) {
        unsigned fx=grain_frame*7u,fy=grain_frame*13u;
        d=garden_dither(sh_px+(int)fx,sh_py+(int)fy);
        // Two levels, not a ramp, and the band is the plant's lowest fifth
        // rather than the screen's. The ramp reached the top of the window on
        // every species, which is why it read as "too wide" -- it was grading
        // the glass, and the plant is not the same shape as the glass.
        //
        // DENSITY steps and not amplitude, because a density step is not a
        // brightness step: the grain is zero-mean and bipolar, so halving how
        // many pixels carry it changes the texture without moving the mean.
        // That is what lets the seam be crossed without a line appearing --
        // an amplitude step at these magnitudes would put one.
        // Evaluated top down and short-circuited: most plant pixels are above
        // the band and cost one seam hash, not three.
        int lv;
        if(sh_py<grain_top+grain_seam(sh_px,0))      lv=0;
        else if(sh_py<grain_b1+grain_seam(sh_px,1))  lv=FLOWER_GRAIN_LOW;
        else if(sh_py<grain_b2+grain_seam(sh_px,2))  lv=FLOWER_GRAIN_MID;
        else                                         lv=FLOWER_GRAIN_MAX;
        // The selector's low bits are an arithmetic phase, not a second noise
        // function: garden_dither carries the two bits that matter and this
        // spreads them over 256 so the density can be graded smoothly instead
        // of in four visible steps.
        unsigned h=(unsigned)(sh_px*37+sh_py*17+(int)grain_frame*29);
        int sel=d*64+(int)(h&63u);
        if(sel<lv) {
            // AMPLITUDE IS A SEPARATE KNOB FROM DENSITY, and only one of them
            // was wrong. The density gradient measured correctly on the board
            // -- 200 to 300 leaf pixels a frame in the lower bands, graded --
            // and the effect was still invisible, because half the perturbed
            // pixels moved by one step of 63 on a leaf sitting at 22 of 63.
            //
            // The error was one inference. "A perturbation smaller than one
            // quantisation step does nothing" is true, and it is why the first
            // two attempts were invisible. But one step is the smallest amount
            // that has ANY effect -- it is not the smallest amount that can be
            // SEEN, and I carried the first over as if it implied the second.
            //
            // The two effects want opposite things. The rounding dither above
            // must NOT be seen, so one step is exactly right there. Grain
            // exists to be seen, so it takes several -- and equal counts of
            // steps in each channel, which means twice the absolute amount in
            // the three-bit channels as in the six-bit one.
            //
            // Bipolar, unlike the dither: noise adds and subtracts. A one-sided
            // perturbation at this amplitude would lift the foliage as well as
            // roughen it.
            // Sign from the hash, magnitude fixed. Spreading the magnitude
            // over the hash's four values put half the grain pixels at one
            // step, which is the amount that was already established as
            // invisible -- so half the density was being spent on nothing.
            // Density already decides WHETHER a pixel is grained; there is no
            // reason for the amount to vary as well.
            // Sign from a bit the SELECTOR does not use. Taking it from d
            // coupled the two: at the lighter level only the low d values pass
            // the threshold, so every grained pixel there would have darkened
            // and the band would have read as a smudge rather than as noise.
            // At the heavy level enough of d passed for the bias to hide, which
            // is exactly the kind of defect that survives being looked at.
            int k=(h&64u?1:-1)*FLOWER_GRAIN_AMP*3;
            g+=k;r+=k*2;b+=k*2;
        }
        return rgb(r,g,b);
    }
    r+=d*2;b+=d*2;g+=d;
#endif
    return rgb(r,g,b);
}
static uint16_t shade(V n,int petal,V hit) {
    unsigned material=petals[petal].material;
    // By material and not by geometry or colour. The enum already says which
    // parts are foliage, so the test is exact and costs a compare -- and a
    // green petal is not a leaf, which any approximation by colour would get
    // wrong on half the species.
    sh_grain=material==LEAF||material==HERB||material==FILAMENT;
    bool inside=n.z<0;
    n=normal(n);
    if(inside)n=mul(n,-1);
    float diffuse=POS(dot(n,(V){-.36f,.48f,.8f}));
    float rim=1-POS(n.z);rim*=rim;
    float spec=POS(dot(n,(V){-.19f,.25f,.949f}));
    spec*=spec;spec*=spec;spec*=spec;spec*=spec;
    {
        const Petal *p=&petals[petal];V local=add(hit,mul(p->c,-1));
        float longitudinal=DIVR(dot(local,p->axis[0]),p->inv_radius[0],p->radius[0]);
        float transverse=DIVR(dot(local,p->axis[1]),p->inv_radius[1],p->radius[1]);
        float light=.30f+.66f*diffuse;
        if(inside)light*=material==GOLD?.8f:.52f;
        if(p->shape==2)light=.55f+.43f*diffuse;
        float r=0,g=0,b=0;
        if(material==LEAF) {
            float vein=POS(1-fabsf(transverse)*12)*.16f;
            r=22;g=105+vein*160;b=53+vein*100;spec*=.35f;
        } else if(material==GOLD) {
            r=255;g=165+25*longitudinal;b=13;
            if(p->shape) {
                g=220;b=47;
                // Keep the corona's inner wall shaded. Overwriting both sides
                // with the same bright floor erased its depth at small size.
                light=inside?.38f+.30f*diffuse:.62f+.34f*diffuse;
                spec*=.3f;
            }
        }
        else if(material==CORONA) {
            // Surface-bound light, not a postprocess bloom: no extra rays,
            // texture or buffer. axis[1] runs from the throat to the mouth.
            if(p->shape) {
                float t=(transverse+1)*.5f;
                float streak=POS(1-fabsf(longitudinal-.20f)*4.5f);
                streak*=streak;
                float echo=POS(1-fabsf(longitudinal+.52f)*9);
                float flow=(streak+.32f*echo)*(.25f+.75f*t);
                float lip=POS((t-.80f)*5);
                float wall=inside?.40f+.32f*diffuse:.48f+.36f*diffuse;
                return rgbd(255*wall+145*flow+100*lip,
                           (150+70*t)*wall+150*flow+100*lip,
                           (8+34*t)*wall+100*flow+75*lip);
            }
            // Recessed luminous throat; the side wall retains an amber shadow.
            float core=POS(1-longitudinal*longitudinal-transverse*transverse);
            return rgbd(224+31*core,159+79*core,27+124*core);
        }
        else if(material==ROSE) {r=242;g=47+35*longitudinal;b=104+32*longitudinal;}
        else if(material==VIOLET) {r=139+34*longitudinal;g=65+20*longitudinal;b=235;}
        else if(material==BLUE) {r=145+25*longitudinal;g=190+20*longitudinal;b=255;light=.55f+.4f*diffuse;spec*=.4f;}
        else if(material==HERB) {r=80;g=161;b=69;light=.55f+.4f*diffuse;spec*=.2f;}
        else if(material==FILAMENT) {
            // A dark anther on the pale filament uses one part, not two.
            bool tip=longitudinal>.65f;
            r=tip?29:226;g=tip?24:233;b=tip?43:217;spec*=.2f;
        }
        else if(material==RED) {r=244;g=35+12*longitudinal;b=55;}
        else if(material==INK) {r=29;g=24;b=43;spec*=.2f;}
        else if(material==CHECKER) {
            // Local coordinates keep the chequering on the bell as it sways.
            // No texture image or extra geometry; only this material pays.
            int tile=(int)floorf((longitudinal+1)*4)+(int)floorf((transverse+1)*5);
            float pale=(tile&1)?1.0f:0.0f;
            // Aubergine ground with subdued violet tessellation. The former
            // pale pink squares overwhelmed both the colour and the volume.
            r=88+27*pale;g=32+14*pale;b=109+29*pale;
            light=(inside?.30f:.48f)+.48f*diffuse;
            spec*=.15f;
        }
        else if(material==SEED) {
            int x=clampi((int)(16+15*longitudinal),0,31),y=clampi((int)(16+15*transverse),0,31);
            float seed=DIVR(seed_map[y*32+x],1.0f/255,255.0f);
            r=50+seed*72;g=25+seed*43;b=12+seed*16;spec*=.1f;
        } else {
            r=225;g=239;b=229;
            if(material==INNER&&longitudinal>.12f) {r=80;g=155;b=75;}
            spec*=.55f;
        }
        return rgbd(r*light+spec*65+rim*16,g*light+spec*65+rim*20,b*light+spec*70+rim*23);
    }
}
// Bell radius is a smooth cubic profile sampled into six conical bands. Each
// band has an analytic ray intersection; the bottom remains open. Both roots
// are considered so the inner-facing far wall can be seen through the mouth.
#ifdef FLOWER_BELL_CHECK
// Host-only bookkeeping: how often the test fires, and whether it ever fired on
// a visit that the full six-band walk would have turned into a hit. The second
// number is the whole proof, and it has to be zero.
unsigned bell_visits_seen,bell_rejected,bell_rejected_wrongly;
// Of the visits the cylinder does not reject and that still miss: which stage
// threw them away. bell_reject can only be tightened towards whichever of these
// is large, so this is the measurement that chooses the next test rather than
// the next test being chosen and then justified.
unsigned bell_miss_disc,bell_miss_height,bell_miss_depth,bell_miss_clip;
// How many times a visit accepts an intersection. The normal used to be
// built on every acceptance and is now built once for the winner, so this
// ratio is the whole of what that trade costs or saves in arithmetic.
unsigned bell_accepts,bell_hit_visits;
#endif
// A bell visit costs about 2,750 cycles on the device -- six latitude bands
// walked unconditionally, each with a discriminant, a software square root and
// a pair of divisions -- and 59-64% of them miss. This is the test that stops
// paying for those.
//
// It is a bounding cylinder, capped: if the ray never comes within the bell's
// widest radius of its axis *while it is at a height the bell occupies*, then
// no band's cone can be met. Both halves are needed and the second was added
// after measuring -- the radial half alone rejected 21.8% of visits, and of the
// survivors that still missed, 38-48% had met a cone outside its band's height.
//
// Conservative by construction: it bounds every band by the widest one and
// ignores which band a height belongs to, so the set it rejects is a subset of
// the misses. Cheap by construction too -- about fourteen multiply-adds, no
// division and no square root, because 1/a0 and 1/d[1] belong to the part and
// are taken once a frame in petal_reciprocals.
//
// "By construction" is not a measurement, which is the whole lesson of this
// file's history; tools/test_bell_reject.c runs the full six-band walk anyway
// and counts the visits where the test said no and the walk said yes.
// __attribute__((unused)) because FLOWER_NO_BELL_REJECT is a build in which
// nothing calls this, and that build has to compile: it is the one that
// recovers the device measurement this rejection never got, having shipped
// inside a commit that was measuring something else.
static bool __attribute__((unused))
bell_reject(const Petal *p,const float *o) {
    const float *d=p->bd;
    float b0=o[0]*d[0]+o[2]*d[2];
    float c0=o[0]*o[0]+o[2]*o[2];
    if(p->ba0<=0)return c0>bell_rmax2;      /* the ray runs along the axis */
    float zc=-b0*p->inv_a0;                 /* where the ray passes closest */
    // ...but only the part of the ray where the bell has any height at all
    // counts. Measured on the host: of the visits the radial test alone let
    // through and that still missed, 38-48% met a cone at a height the bell
    // does not reach. Clamping the closest approach into the height window is
    // what catches those, and it needs no division because 1/d[1] and 1/a0
    // belong to the part rather than the pixel.
    if(p->inv_d1!=0) {
        float za=(-1-o[1])*p->inv_d1,zb=(1-o[1])*p->inv_d1;
        float zlo=za<zb?za:zb,zhi=za<zb?zb:za;
        if(zc<zlo)zc=zlo;else if(zc>zhi)zc=zhi;
    } else if(o[1]<-1||o[1]>1) return true; /* height fixed, and outside it */
    return c0+(2*b0+p->ba0*zc)*zc>bell_rmax2;
}
// `ob` carries the part of o[] that depends on the row rather than the column;
// see petal_reciprocals. bell_hit_at below is the form that takes a bare
// (dx,dy), for callers outside the span walk.
static bool bell_hit(const Petal *p,float dx,const float *ob,float *best,V *norm) {
    float o[3],d[3];
#ifdef FLOWER_NO_SPAN_AFFINE
    V origin={dx,ob[3],0};
    for(int j=0;j<3;j++)o[j]=DIVR(dot(origin,p->axis[j]),p->inv_radius[j],p->radius[j]);
#else
    for(int j=0;j<3;j++)o[j]=dx*p->oax[j]+ob[j];
#endif
    for(int j=0;j<3;j++)d[j]=p->bd[j];
#ifdef FLOWER_BELL_CHECK
    bell_visits_seen++;
    bool rejected=bell_reject(p,o);
    if(rejected)bell_rejected++;
#elif !defined(FLOWER_NO_BELL_REJECT)
    if(bell_reject(p,o))return false;
#endif
    bool found=false;
    // The normal is built here rather than once after the loop, and that was
    // measured rather than assumed. Hoisting it looked like the best structural
    // idea available: *norm is overwritten on every acceptance and only the last
    // survives, so recording z and the band and building it once is exactly
    // equivalent -- it measured bit-identical over 14 species. It was reverted
    // because all three reasons for it turned out to be false.
    //
    //   Acceptances per hit visit are 1.00 to 1.01 (tools/test_bell_reject.c
    //   counts them). The normal was already being built once per hit, so there
    //   was nothing to save; hoisting it *added* the recomputation of u, v and w.
    //
    //   The three accumulated components were never competing for registers.
    //   *norm is a pointer parameter, so GCC stores them straight to memory as
    //   they are produced -- they were a store destination, not a live set.
    //   That was the error in the hand enumeration that motivated this.
    //
    //   The disassembly agrees: hoisted, ray_row went 507 instructions to 521
    //   and its float memory traffic did not move (144 to 145 ops).
    //
    // Leaving this note instead of the change, because the reasoning for it is
    // sound and somebody will have it again.
#ifdef FLOWER_BELL_CHECK
    bool saw_root=false,saw_height=false,saw_depth=false,saw_clip=false;
#endif
    // The cloche fits inside the ordinary bell's conservative 1.12 bound.
    const float *slopes=p->shape==FLOWER_SHAPE_CLOCHE?flower_cloche_slopes:bell_slopes;
    const float *offsets=p->shape==FLOWER_SHAPE_CLOCHE?flower_cloche_offsets:bell_offsets;
    for(int band=0;band<LAT;band++) {
#ifdef FLOWER_NO_BAND_HOIST
        float lo=-1+2.0f*band/LAT,hi=-1+2.0f*(band+1)/LAT;
#else
        float lo=bell_lo[band],hi=bell_hi[band];
#endif
        float slope=slopes[band],offset=offsets[band];
        float r=slope*o[1]+offset,dr=slope*d[1];
        float a=d[0]*d[0]+d[2]*d[2]-dr*dr;
        float b=o[0]*d[0]+o[2]*d[2]-r*dr;
        float c=o[0]*o[0]+o[2]*o[2]-r*r;
        float roots[2];int nr=0;
        if(fabsf(a)<1e-7f) {if(fabsf(b)>1e-7f)roots[nr++]=-c/(2*b);}
        else {
            float disc=b*b-a*c;if(disc<0)continue;
            // The two roots share a divisor, so they share one divide. The
            // square root is timed because there can be six of them in a
            // visit, at 174 measured cycles each, and that is the largest
            // thing in bell_hit that has a name.
            //
            // This counter used to sit around ray_row's `dy` and was called
            // `div`, on the theory that it was pricing a software division. It
            // was not: the numerator depends only on y, so the compiler hoists
            // the divide out of the petal loop and the brackets contained a
            // subtraction. It reported 3-4 cycles, which was true and told
            // nobody anything, and an estimate of "150-250 cycles per software
            // division" was built on top of it and used to justify a plan. A
            // counter that reports a plausible wrong number is worse than no
            // counter; this one was moved rather than deleted because there is
            // a real question here, but the name it had has to go with it.
#ifdef ESP_PLATFORM
            PROF_FENCE;uint32_t bs=esp_cpu_get_cycle_count();PROF_FENCE;
#endif
#ifdef FLOWER_BELL_CHECK
            saw_root=true;
#endif
            float sd=sqrtf(disc);
#ifdef ESP_PLATFORM
            PROF_FENCE;prof_div+=esp_cpu_get_cycle_count()-bs;prof_divn++;PROF_FENCE;
#endif
#ifdef FLOWER_DIV_EXACT
            roots[nr++]=(-b+sd)/a;roots[nr++]=(-b-sd)/a;
#else
            float inva=1.0f/a;roots[nr++]=(-b+sd)*inva;roots[nr++]=(-b-sd)*inva;
#endif
        }
        for(int k=0;k<nr;k++) {
            float z=roots[k],v=o[1]+d[1]*z;
#ifdef FLOWER_BELL_CHECK
            if(v>=lo&&v<=hi)saw_height=true;
            if(v>=lo&&v<=hi&&z+p->c.z>*best)saw_depth=true;
#endif
            if(v<lo||v>hi||z+p->c.z<=*best)continue;
            float u=o[0]+d[0]*z,w=o[2]+d[2]*z;
            // A calla's spathe is asymmetrically open, exposing its spadix.
#ifdef FLOWER_BELL_CHECK
            if(p->shape==2&&(v>.28f-.8f*w||v>1-.65f*u*u))saw_clip=true;
#endif
            if(p->shape==2&&(v>.28f-.8f*w||v>1-.65f*u*u))continue;
            V n=add(add(mul(p->axis[0],DIVR(u,p->inv_radius[0],p->radius[0])),
                        mul(p->axis[1],DIVR(-(slope*v+offset)*slope,p->inv_radius[1],p->radius[1]))),
                    mul(p->axis[2],DIVR(w,p->inv_radius[2],p->radius[2])));
#ifdef FLOWER_BELL_CHECK
            bell_accepts++;
#endif
            *best=z+p->c.z;*norm=n;found=true;
        }
    }
#ifdef FLOWER_BELL_CHECK
    if(found)bell_hit_visits++;
    if(rejected&&found)bell_rejected_wrongly++;
    if(!rejected&&!found) {
        // In the order the walk applies them, so each count is "got this far
        // and no further".
        if(!saw_root)        bell_miss_disc++;    /* outside every cone, radially */
        else if(!saw_height) bell_miss_height++;  /* met a cone outside its band */
        else if(!saw_depth)  bell_miss_depth++;   /* met it behind what is drawn */
        else if(saw_clip)    bell_miss_clip++;    /* the calla's open spathe */
    }
#endif
    return found;
}
// The dissolve, on the way out of shade(). It mixes towards `sky` rather than
// towards what is already in the pixel, so two overlapping petals blend once
// each instead of compounding. The endpoints are returned untouched, so a fully
// opaque frame is bit-identical to one drawn with no rotation at all.
static uint16_t dissolve(uint16_t sky,uint16_t lit) {
    if(bloom_fade>=1)return lit;
    if(bloom_fade<=0)return sky;
    unsigned f=(unsigned)(bloom_fade*256),g=256-f;
    unsigned r=(((sky>>11)&31)*g+((lit>>11)&31)*f)>>8;
    unsigned gr=(((sky>>5)&63)*g+((lit>>5)&63)*f)>>8;
    unsigned b=((sky&31)*g+(lit&31)*f)>>8;
    return (uint16_t)(r<<11|gr<<5|b);
}
// ob[0..2] is dy's contribution to o[], constant along a row; ob[3] carries dy
// itself so that FLOWER_NO_SPAN_AFFINE can rebuild the original expression.
static void bell_row_terms(const Petal *p,float dy,float *ob) {
    for(int j=0;j<3;j++)ob[j]=dy*p->axis[j].y*p->inv_radius[j];
    ob[3]=dy;
}
static bool __attribute__((unused))
bell_hit_at(const Petal *p,float dx,float dy,float *best,V *norm) {
    float ob[4];bell_row_terms(p,dy,ob);
    return bell_hit(p,dx,ob,best,norm);
}
// Tilt-shift, on the plant.
//
// The subject is the flower, so the flower is what goes in and out of focus --
// sharp across a band and soft above and below it, which is the whole of what
// makes the look. It is applied here and not to the garden: the mist behind is
// already a soft field, and softening it further reads as fog rather than as a
// shallow depth of field. (This was got the wrong way round first, on the
// strength of the scene being called FLOWER.)
//
// Two symmetric passes of a one-pole filter along x, over only the part of the
// row the plant covered. Symmetric because one pass alone smears in one
// direction and reads as motion rather than defocus; horizontal only because a
// vertical pass needs a second row and this board has one shared strip buffer.
// Tilt-shift is recognised by the GRADIENT of sharpness rather than by the
// blur's isotropy, so one axis is usually enough.
//
// Packed averaging, so no channel is ever unpacked: (a^b)&0xF7DE >> 1 plus a&b
// is the mean of two RGB565 pixels in four operations, exact for the even bits
// and one low bit short on the odd ones -- which is a dither of sorts, and at
// this amplitude is exactly the "use dither" the effect was asked to use.
static void ray_row(uint16_t *row,int y) {
    // Preserve the woodland under overlapping petals during the dissolve.
    // Automatic storage only; no extra full-frame or persistent pixel buffer.
#ifdef ESP_PLATFORM
    PROF_FENCE;uint32_t p0=esp_cpu_get_cycle_count();PROF_FENCE;
#endif
    uint16_t backdrop[FW];memcpy(backdrop,row+X0,sizeof backdrop);
#ifdef ESP_PLATFORM
    PROF_FENCE;prof_pre+=esp_cpu_get_cycle_count()-p0;
    uint32_t c0=esp_cpu_get_cycle_count();PROF_FENCE;
#endif
    for(unsigned i=0;i<count;i++) {
        const Petal *p=&petals[i];if(y<p->ymin||y>p->ymax)continue;
        // One __divsf3, timed on its own. Every float `/` in this file is a
        // call into a ROM software routine -- the FPU on this part has no
        // divide instruction and the compiler never emits the seed sequence --
        // and the whole remaining plan turns on what one of them costs.
        float dy=DIVR(65-(y+.5f),cam_inv,cam_s)+cam_y-p->c.y;
        // PIE candidate (unmeasured): for ellipsoids, b, c and discriminant d
        // are polynomials across x. A bounded fixed-point 8-pixel rejection
        // pass could skip misses before scalar sqrt/depth/normal/shading.
        // PIE is not floating-point SIMD: retain conservative hit masks near
        // d=0, prove ranges, and compare silhouettes/depth with this reference.
        // Bell clipping is a separate path; measure it before extending this.
#ifdef ESP_PLATFORM
        prof_visits+=(uint32_t)(p->xmax-p->xmin+1);
        PROF_FENCE;uint32_t x0=esp_cpu_get_cycle_count();PROF_FENCE;
#endif
        float ob[4];
        if(p->shape)bell_row_terms(p,dy,ob);
        // The band, and it is narrower than it was first built.
        //
        // The obvious version stepped the visit loop itself -- one ray in four,
        // the answer copied across the run -- which takes a quarter off BOTH of
        // ray_row's large terms at once, since `span` is per visit and `shade`
        // is per hit. It also deletes anything thinner than the step. A stem is
        // one or two pixels wide and has a one-in-four chance of being sampled,
        // so the plant lost its stem in the defocused bands and the assertion
        // in tools/test_flower.c that every species reaches the bottom edge
        // failed on the first species it tried. That is not an artefact to tune
        // away: a silhouette with holes in it is a different plant.
        //
        // So the geometry is evaluated at every pixel and only the SHADING is
        // sampled. Hits, depth and silhouette are bit-identical to the sharp
        // build; what a defocused run shares is one pixel's colour. `shade` is
        // the larger of the two terms anyway -- 5.03 ms against span's 4.30 --
        // and this half of the win is the half that is safe.
        //
        // The grid is global (x & ~(rate-1)) rather than per petal: two petals
        // overlapping a run must reuse on the same boundaries, or the seam
        // between them moves with the plant.
        for(int x=p->xmin;x<=p->xmax;x++) {
            float dx=DIVR(x+.5f-180,cam_inv,cam_s)+cam_x-p->c.x;
            if(p->shape) {
                float z=depth[x-X0];V n;
                // bell_hit is timed on every visit, not every hit, because
                // that is how it runs: six conical bands, each with its own
                // discriminant, square root and pair of divisions, and
                // 59-64% of them miss. Dividing ray_row by `hits` hides it
                // completely.
#ifdef ESP_PLATFORM
                PROF_FENCE;uint32_t v0=esp_cpu_get_cycle_count();PROF_FENCE;
                bool got=bell_hit(p,dx,ob,&z,&n);
                PROF_FENCE;prof_bell+=esp_cpu_get_cycle_count()-v0;prof_belln++;PROF_FENCE;
                if(got) {
#else
                if(bell_hit(p,dx,ob,&z,&n)) {
#endif
                    depth[x-X0]=z;
#ifdef ESP_PLATFORM
                    prof_hits++;
                    PROF_FENCE;uint32_t b0=esp_cpu_get_cycle_count();PROF_FENCE;
#endif
                    // Every hit is shaded. The run sharing existed only because
                    // the row was about to be blurred; with no blur a coarsely
                    // shaded row is simply coarse, so it goes with it -- and the
                    // 1.7 ms it was giving back goes with it too.
                    sh_px=x;sh_py=y;
            uint16_t lit=shade(n,i,(V){dx+p->c.x,dy+p->c.y,z});
#ifdef ESP_PLATFORM
                    PROF_FENCE;prof_shade+=esp_cpu_get_cycle_count()-b0;PROF_FENCE;
#endif
                    row[x]=dissolve(backdrop[x-X0],lit);
                }
                continue;
            }
            float b=p->q[4]*dx+p->q[5]*dy;
            float c=p->q[0]*dx*dx+2*p->q[3]*dx*dy+p->q[1]*dy*dy-1;
            float d=b*b-p->q[2]*c;if(d<0)continue;
            // sqrtf is not one instruction on this part. It resolves to an
            // 88-instruction software routine behind a two-level call, and
            // normal() inside shade() runs another one and a soft-float divide
            // on top; this is here to find out what that actually costs before
            // anybody replaces it.
#ifdef ESP_PLATFORM
            PROF_FENCE;uint32_t s0=esp_cpu_get_cycle_count();PROF_FENCE;
#endif
            float root=sqrtf(d);
#ifdef ESP_PLATFORM
            PROF_FENCE;prof_sqrt+=esp_cpu_get_cycle_count()-s0;prof_sqrtn++;PROF_FENCE;
#endif
            float dz=(-b+root)*p->invzz,z=dz+p->c.z;
            if(z<=depth[x-X0])continue;
            depth[x-X0]=z;
#ifdef ESP_PLATFORM
            prof_hits++;
#endif
            V n={p->q[0]*dx+p->q[3]*dy+p->q[4]*dz,
                 p->q[3]*dx+p->q[1]*dy+p->q[5]*dz,
                 p->q[4]*dx+p->q[5]*dy+p->q[2]*dz};
#ifdef ESP_PLATFORM
            PROF_FENCE;uint32_t h0=esp_cpu_get_cycle_count();PROF_FENCE;
#endif
            sh_px=x;sh_py=y;
            uint16_t lit=shade(n,i,(V){dx+p->c.x,dy+p->c.y,z});
#ifdef ESP_PLATFORM
            PROF_FENCE;prof_shade+=esp_cpu_get_cycle_count()-h0;PROF_FENCE;
#endif
            row[x]=dissolve(backdrop[x-X0],lit);
        }
#ifdef ESP_PLATFORM
        PROF_FENCE;prof_span+=esp_cpu_get_cycle_count()-x0;prof_spann++;PROF_FENCE;
#endif
    }
#ifdef ESP_PLATFORM
    PROF_FENCE;prof_scan+=esp_cpu_get_cycle_count()-c0;PROF_FENCE;
#endif
}
#if FLOWER_HORROR
// Packed averaging: the mean of two RGB565 pixels in four operations, exact on
// the even bits and one low bit short on the odd ones -- which is a dither of
// sorts at this amplitude. Written for the defocus and outliving it, because
// the blowout and its halo are made of the same primitive.
static inline uint16_t avg565(uint16_t a,uint16_t b) {
    return (uint16_t)((((a^b)&0xF7DEu)>>1)+(a&b));
}
// One pole. Applying avg565 again against the accumulator raises the
// coefficient rather than the count: once is 1/2, twice 3/4, three times 7/8.
static inline uint16_t pole565(uint16_t acc,uint16_t c,int mixes) {
    uint16_t v=avg565(acc,c);
    for(int m=1;m<mixes;m++)v=avg565(v,acc);
    return v;
}
// The blowout's halo. It was a standing effect with its own budget and is now
// part of an event's price -- the silhouette seeding and the brightness gate
// stay, because a blown frame that is uniform mush is not a picture of
// anything, and those two are what keep the light coming off the bright parts.
//
// depth[] is still this row's, so the true outline is in scope here exactly as
// it was in ray_row.
static void horror_bloom_row(uint16_t *row) {
        for(int x=0;x<FW;x++) {
            int cov=depth[x]>-999.0f;
            if(!cov)continue;
            int lft=x==0||depth[x-1]<=-999.0f;
            int rgt=x==FW-1||depth[x+1]<=-999.0f;
            if(!lft&&!rgt)continue;
            uint16_t src=row[X0+x];
            if((int)((src>>5)&63u)<FLOWER_BLOOM_MIN)continue;
            for(int side=0;side<2;side++) {
                if(side?!rgt:!lft)continue;
                int step=side?1:-1;
                uint16_t acc=src;
                for(int k=1;k<=FLOWER_BLOOM_R;k++) {
                    int xx=x+k*step;
                    if(xx<0||xx>=FW)break;
                    if(depth[xx]>-999.0f)break;   /* stop at the next petal */
                    // One pole running outward: the carried colour decays into
                    // the background it crosses, so the halo fades with
                    // distance at one average and one store a pixel.
                    acc=avg565(acc,row[X0+xx]);
                    row[X0+xx]=acc;
#ifdef ESP_PLATFORM
                    prof_horrorn++;
#endif
                }
            }
        }
}
#endif
#if FLOWER_HORROR
// How far into an event we are, 0 off, 255 full. Hard in and a decay out: a
// symmetric envelope reads as a transition, and a cut followed by a fade reads
// as something having happened. The dissolve is deliberately avoided rather
// than ridden -- the species change is already a moment with a meaning, and
// putting the event on top of it would make the two one thing.
static uint8_t horror_level;
static float horror_clock;
static void horror_tick(float dt) {
    if(horror_level) {
        int step=(int)(255.0f*dt*1000.0f/FLOWER_HORROR_MS)+1;
        horror_level=(uint8_t)(horror_level>step?horror_level-step:0);
        return;
    }
    horror_clock+=dt;
    if(horror_clock>=FLOWER_HORROR_EVERY) {
        horror_clock=0;
        horror_level=255;               /* the cut in */
    }
}
// One row of it. No threshold anywhere, which is the point: everything blooms.
//
// Order is smear, blow out, invert -- and the inversion is LAST because it is
// the thing the eye reads. Inverting first and then blooming would spread the
// negative's own bright areas, which are the scene's dark ones, and the result
// is a soft grey rather than a burnt photograph.
static void horror_row(uint16_t *row) {
    uint16_t acc=row[0];
    for(int x=0;x<W;x++) {
        // A wide, undisciplined smear. Pole 7/8, so it carries most of the way
        // across the row and every bright thing drags a halo behind it.
        acc=pole565(acc,row[x],3);
        uint16_t blown=avg565(avg565(row[x],acc),0xFFFFu);
        uint16_t hit=(uint16_t)(blown^0xFFFFu);
        // The envelope, in the same primitive: three steps toward the effect
        // rather than a lerp that would need the channels unpacked.
        uint16_t out=row[x];
        int lvl=horror_level;
        if(lvl>200)out=hit;
        else if(lvl>120)out=avg565(hit,avg565(hit,out));
        else if(lvl>40)out=avg565(hit,out);
        else if(lvl)out=avg565(out,avg565(hit,out));
        row[x]=out;
#ifdef ESP_PLATFORM
        prof_horrorn++;
#endif
    }
}
#endif
void flower_draw(uint16_t *pixels,int y,int height) {
    // Held for FLOWER_GRAIN_JITTER frames at a time, and a different multiplier
    // per boundary so the three are uncorrelated. The bands are 8 rows and
    // wider, so +-2 rows of jitter plus +-2 of rag cannot carry one boundary
    // past the next.
    //
    // At DRAW time and not at prepare. Deriving it in flower_prepare made the
    // plant a function of the frame counter as it stood when the camera was
    // set rather than of flower_grain_frame, so the same pose drawn whole and
    // drawn in strips disagreed -- caught by strip equivalence on the first
    // run, which is the assertion's entire job.
    {
        for(int b=0;b<3;b++) {
            // PHASE per boundary, not just a different constant: offsetting the
            // frame before the divide staggers WHEN each seam moves as well as
            // where to. Three seams stepping on the same frame is the lockstep
            // this is meant to avoid, however uncorrelated their values are.
            unsigned t=(grain_frame+(unsigned)b*4u)/(unsigned)FLOWER_GRAIN_JITTER;
            // And a real mix. Multiplying one t by three nearby constants left
            // the high bits correlated -- the first version produced [-2,-2,-2]
            // and [0,0,0], which is exactly the shared offset it was written to
            // prevent, arrived at by a different route.
            unsigned h=(t*2654435761u)^((unsigned)b*0x85EBCA6Bu+0x9E3779B9u);
            h^=h>>15;h*=0x2545F491u;h^=h>>13;
            grain_jit[b]=(int)(h%5u)-2;
        }
    }
    if(!pixels||y<0||height<0||y>H||height>H-y)return;
    GardenFrame fallback={0};   /* the swarm carries state; zero is its valid start */
    const GardenFrame *garden;
    if(seed_map)garden=(const GardenFrame*)(seed_map+32*32);
    else {garden_prepare(&fallback,elapsed);garden=&fallback;}
    for(int j=0;j<height;j++) {
        int py=y+j;uint16_t *row=pixels+j*W;
#ifdef ESP_PLATFORM
        PROF_FENCE;uint32_t t0=esp_cpu_get_cycle_count();PROF_FENCE;
        garden_row_blend(row,py,garden,bloom_garden_old_seed,bloom_garden_mix);
        PROF_FENCE;uint32_t t1=esp_cpu_get_cycle_count();PROF_FENCE;
        prof_garden+=t1-t0;
#else
        garden_row_blend(row,py,garden,bloom_garden_old_seed,bloom_garden_mix);
#endif
        // Without the block there is no flower, but there is still a sky. A
        // background that cannot allocate should look plain, not crash.
        // The roots continue beyond the bottom edge, like the garden grasses.
        // A margin at y=119 visibly severed every stem above the ground.
        if(py<12||!depth) {
#ifdef ESP_PLATFORM
            PROF_FENCE;prof_total+=esp_cpu_get_cycle_count()-t0;PROF_FENCE;
#endif
            continue;
        }
        for(int x=0;x<FW;x++)depth[x]=-1000;
#if GARDEN_MOTE_ONLY
        // The plant is background too, as far as that diagnostic is concerned.
        // It is skipped here rather than inside ray_row because ray_row's
        // backdrop[] is a copy of the garden row -- motes included -- and every
        // petal pixel is dissolve(backdrop, lit). Left running, it would paint
        // over the very pixels the build exists to look at, and during a
        // species swap it would blend the outgoing plant into the beam, which
        // is the shape this diagnostic is trying to tell the motes apart from.
        (void)0;
#else
        ray_row(row,py);
#endif
#if FLOWER_HORROR
        // Last, over everything: the garden, the plant and the swarm. This is
        // the whole frame blooming, not the plant -- the outside-the-silhouette
        // constraint belongs to the tasteful bloom and does not apply here.
        if(horror_level) {
#ifdef ESP_PLATFORM
            PROF_FENCE;uint32_t hh=esp_cpu_get_cycle_count();PROF_FENCE;
#endif
            horror_bloom_row(row);
            horror_row(row);
#ifdef ESP_PLATFORM
            PROF_FENCE;prof_horror+=esp_cpu_get_cycle_count()-hh;PROF_FENCE;
#endif
        }
#endif
#ifdef ESP_PLATFORM
        PROF_FENCE;prof_total+=esp_cpu_get_cycle_count()-t0;PROF_FENCE;
#endif
    }
#ifdef ESP_PLATFORM
    if(y+height>=H&&++prof_frames>=60) {
        double tot=prof_total/240000.0/prof_frames,gar=prof_garden/240000.0/prof_frames;
        // The vector half of garden_row, so that `garden` can be split into
        // what the PIE kernel costs and what the still-scalar trunks, canopy
        // and grass cost. Nobody has ever measured the second number, and
        // after the kernel landed it is the larger half of the two.
        double pix=garden_prof_pixels()/240000.0/prof_frames;
        // The mote touch-up, timed where it runs. `moterows` is the count the
        // per-row figure is divided by, printed rather than assumed, because
        // the last two attributions in this file both divided by a number
        // nobody had counted.
        uint32_t moterows=0;
        uint32_t motecy=garden_prof_motes(&moterows);
        double mot=motecy/240000.0/prof_frames;
        // visits are the pixels the rejection arithmetic touches; hits are the
        // ones that reach sqrtf, the normal and shade(). The two have very
        // different unit costs -- roughly 20 cycles against 200 -- so which of
        // them dominates decides whether narrowing the spans is worth anything
        // at all, and nobody has ever counted the second one.
        // Inside ray_row: sqrt is every d>=0 (more than the hits), shade is
        // every hit. Two rsr.ccount either side of each is about 2 cycles on
        // regions of 150 and 1000, so the perturbation is under 2% -- but it
        // is not zero, and the sqrt figure carries the larger share of it.
        // TWO LINES, and the split is the fix rather than a tidy-up.
        //
        // This was one printf with twenty-six conversions. Three arguments for
        // `tilt=` were written after `rest=` while the text sat at the end, so
        // every argument after the twenty-first was consumed by the wrong
        // conversion -- and because a float is promoted through varargs, an
        // integer slot reading a float's bit pattern printed 1072049757 six
        // lines running while everything around it was noise.
        //
        // The damage was not the wrong numbers. It was that a trusted line
        // stopped being trustworthy because something new was added to it: the
        // ray decomposition, which had reconciled all morning, came out beside
        // garbage and could not be used either. So SPLIT is back to exactly the
        // terms it had, and everything added since lives in SPLIT2 where being
        // wrong costs only itself.
        ESP_LOGI("garden","SPLIT frames=%u total=%.2f garden=%.2f pixels=%.2f decor=%.2f "
                 "ray=%.2f visits=%u hits=%u | sqrt=%.2f (%u calls, %u cy) shade=%.2f (%u cy) "
                 "bell=%.2f (%u visits, %u cy) | span=%.2f (%u rows, %u cy/visit) "
                 "bell:sqrt=%.2f (%u calls, %u cy) scan=%.2f pre=%.2f rest=%.2f "
                 // The nesting, because a reader cannot see it and adding the
                 // terms as if they were siblings leaves a hole that looks like
                 // a defect in the instrument. bell:sqrt is timed INSIDE
                 // bell_hit, so it is already in bell, and span is the sum of
                 // its three children plus the per-visit remainder -- which is
                 // what the (visits, cy/visit) pair reports. Checked on the
                 // numbers that prompted this: 16.55 = 0.91 + 4.22 + 8.04 +
                 // 3.38, and the printed 110 cy x 7342 is that 3.38.
                 "(ms/frame; span = sqrt + shade + bell + visits*cy_per_visit, "
                 "and bell:sqrt is inside bell, not beside it; "
                 "total vs kernel= is the check)",
                 prof_frames,tot,gar,pix,gar-pix,tot-gar,
                 prof_visits/prof_frames,prof_hits/prof_frames,
                 prof_sqrt/240000.0/prof_frames,prof_sqrtn/prof_frames,
                 prof_sqrtn?prof_sqrt/prof_sqrtn:0,
                 prof_shade/240000.0/prof_frames,
                 prof_hits?prof_shade/prof_hits:0,
                 prof_bell/240000.0/prof_frames,prof_belln/prof_frames,
                 prof_belln?prof_bell/prof_belln:0,
                 prof_span/240000.0/prof_frames,prof_spann/prof_frames,
                 prof_visits?(prof_span-prof_sqrt-prof_shade-prof_bell)/prof_visits:0,
                 prof_div/240000.0/prof_frames,prof_divn/prof_frames,
                 prof_divn?prof_div/prof_divn:0,
                 (prof_scan-prof_span)/240000.0/prof_frames,
                 prof_pre/240000.0/prof_frames,
                 tot-gar-(prof_scan+prof_pre)/240000.0/prof_frames);
        // species= first, because without it two SPLIT lines are not
        // comparable and every attribution made from a pair of them is a
        // guess. The frame rate ranges from 19.5 to 30.3 fps on this build
        // purely by which plant is on screen -- bell-bearing species spend
        // fourteen to sixteen milliseconds in bell_hit and species with none
        // spend nothing -- so the species is not context for these numbers, it
        // is the largest term in them.
        //
        // The A/B halves land on whatever the rotator is showing, so they are
        // only comparable once averaged over species. Naming the species is
        // what lets that be checked rather than assumed.
        ESP_LOGI("garden","SPLIT2 species=%u view=%d parts=%u | "
                 "motes=%.3f (%u rows/frame, %u cy/row) "
                 "horror=%.3f (%u px/frame, %u cy/px) | "
                 "prep: garden=%.3f seeds=%.3f build=%.3f petals=%.3f (%u cy/part)",
                 (unsigned)bloom_species,bloom_view,prof_ppetaln/prof_frames,
                 mot,moterows/prof_frames,moterows?motecy/moterows:0,
                 prof_horror/240000.0/prof_frames,prof_horrorn/prof_frames,
                 prof_horrorn?prof_horror/prof_horrorn:0,
                 prof_pgarden/240000.0/prof_frames,prof_pseeds/240000.0/prof_frames,
                 prof_pbuild/240000.0/prof_frames,prof_ppetal/240000.0/prof_frames,
                 prof_ppetaln?prof_ppetal/prof_ppetaln:0);
        prof_total=prof_garden=prof_visits=prof_hits=0;prof_frames=0;
        prof_sqrt=prof_sqrtn=prof_shade=prof_bell=prof_belln=0;
        prof_span=prof_spann=prof_div=prof_divn=prof_scan=prof_pre=0;
        prof_pgarden=prof_pseeds=prof_pbuild=prof_ppetal=prof_ppetaln=0;
        prof_horror=prof_horrorn=0;
    }
#endif
}
