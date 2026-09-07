#include "flower.h"
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
#endif

// Orthographic primary rays intersect thin ellipsoids analytically. This is
// actual visibility tracing, but the pearl/glass lighting is an approximation:
// no secondary rays, refraction, or physically based transparency is claimed.
#define W 240
#define H 135
#define X0 120
#define FW 120
#define PETALS 8
#define MAX_PARTS 56
#define LAT 6
#define PI 3.14159265358979323846f
#define SCALE 37.0f
typedef struct { float x,y,z; } V;
typedef struct {
    V c,axis[3];
    float radius[3],inv_radius[3],bd[3],oax[3],ba0,inv_a0,inv_d1,q[6],invzz;
    int xmin,xmax,ymin,ymax;
    unsigned material,shape;
} Petal;
enum { PEARL, HEART, LEAF, IVORY, GOLD, SEED, INNER, ROSE, VIOLET };
// The three arrays below live in the shared scene block, not in .bss: see
// scene_mem.h. Declaring them as pointers rather than arrays is what keeps
// every use site in this file unchanged -- petals[i] reads the same either way.
static Petal *petals;
static uint8_t *seed_map;
static float *depth;
#define FLOWER_BYTES (sizeof(Petal)*MAX_PARTS+sizeof(float)*FW+32*32+sizeof(GardenFrame))
// Any address unique to this file identifies it to the block.
static const char flower_owner;
static unsigned count=PETALS;
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
static float elapsed;

// ---------------------------------------------------------------------------
// The rotation behind the single FLOWER menu row.
//
// One row draws the botanical collection in turn. CRYSTAL is not in the rotation:
// it is the fixture the host test checks its analytic surface against, still
// built and still tested, simply not reachable from the menu.
//
// Changing species makes botanicals() rebuild the whole part list -- 31 parts
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
static float bloom_elapsed;              // seconds the current species has been up
static flower_species_t bloom_species=FLOWER_VALLEY;

// xorshift32, seeded by a constant. Deterministic on purpose: the host test
// drives the same sequence the device does, which is the only way to assert
// that the botanical collection comes up and every swap is hidden. A per-boot seed
// would buy unpredictability nobody asked for and cost the test its evidence.
static uint32_t bloom_rng=0x9e3779b9u;
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
static V add(V a,V b) { return (V){a.x+b.x,a.y+b.y,a.z+b.z}; }
static V mul(V a,float b) { return (V){a.x*b,a.y*b,a.z*b}; }
static float dot(V a,V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static V cross(V a,V b) {return (V){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
// Measured at 1.16 ms of a 22 ms ray_row, so the reciprocal square root that
// would replace this sqrtf-then-divide is NOT worth writing. The idea is
// correct and keeps resurfacing -- software sqrt, two-stage call, a soft-float
// divide after it -- and it was wrong about the magnitude three times before
// the counter settled it. Left here so the next person finds the number
// instead of the reasoning.
static V normal(V a) { float d=dot(a,a);
    return mul(a,1.0f/sqrtf(d>1e-12f?d:1e-12f)); }
static V rotate(V a,float yaw,float pitch) {
    float c=cosf(yaw),s=sinf(yaw),cp=cosf(pitch),sp=sinf(pitch);
    V b={c*a.x-s*a.y,s*a.x+c*a.y,a.z};
    return (V){b.x,cp*b.y-sp*b.z,sp*b.y+cp*b.z};
}
static int clampi(int x,int lo,int hi) { return x<lo?lo:x>hi?hi:x; }
static uint16_t rgb(int r,int g,int b) {
    return (uint16_t)((clampi(r,0,255)>>3)<<11 |
                      (clampi(g,0,255)>>2)<<5 | (clampi(b,0,255)>>3));
}
// Build long axes from endpoints, so stems, leaves and hanging petals share
// the analytic ellipsoid path. No extra mesh storage per botanical part.
static void part(V a,V b,float width,float thick,unsigned material,float yaw,float pitch) {
    if(count>=MAX_PARTS)return;
    Petal *p=&petals[count++];V d=add(b,mul(a,-1));float len=sqrtf(dot(d,d));
    p->c=rotate(mul(add(a,b),.5f),yaw,pitch);
    V u=normal(d),v=normal(cross((V){0,0,1},u)),n=cross(u,v);
    p->axis[0]=rotate(u,yaw,pitch);p->axis[1]=rotate(v,yaw,pitch);p->axis[2]=rotate(n,yaw,pitch);
    p->radius[0]=fmaxf(len*.54f,.01f);p->radius[1]=width;p->radius[2]=thick;
    p->material=material;p->shape=0;
}
static V bezier(V a,V b,V c,float t) {return add(add(mul(a,(1-t)*(1-t)),mul(b,2*t*(1-t))),mul(c,t*t));}
static void stem(V a,V b,V c,int steps,float radius,float yaw,float pitch) {
    for(int i=0;i<steps;i++)part(bezier(a,b,c,(float)i/steps),bezier(a,b,c,(float)(i+1)/steps),radius,radius,LEAF,yaw,pitch);
}
static void bell(V top,float size,float lean,float yaw,float pitch) {
    if(count>=MAX_PARTS)return;
    Petal *p=&petals[count++];
    V down={sinf(lean),-cosf(lean),0},side={cosf(lean),sinf(lean),0};
    p->c=rotate(add(top,mul(down,size*.52f)),yaw,pitch);
    p->axis[0]=rotate(side,yaw,pitch);p->axis[1]=rotate(down,yaw,pitch);p->axis[2]=rotate((V){0,0,1},yaw,pitch);
    p->radius[0]=size*.36f;p->radius[1]=size*.52f;p->radius[2]=size*.36f;
    p->material=IVORY;p->shape=1;
}
static void trumpet(V root,V direction,float length,float radius,unsigned material,unsigned shape,float yaw,float pitch) {
    if(count>=MAX_PARTS)return;
    Petal *p=&petals[count++];V axis=normal(direction);
    V side=normal(cross(axis,fabsf(axis.z)>.9f?(V){0,1,0}:(V){0,0,1}));
    p->c=rotate(add(root,mul(axis,length*.5f)),yaw,pitch);
    p->axis[0]=rotate(side,yaw,pitch);p->axis[1]=rotate(axis,yaw,pitch);
    p->axis[2]=rotate(cross(side,axis),yaw,pitch);
    p->radius[0]=radius;p->radius[1]=length*.5f;p->radius[2]=radius;
    p->material=material;p->shape=shape;
}
// The amplitudes of the botanical animation, named rather than inline so that
// tools/flower_stale.c can price them. Raising them costs no frame time at all:
// ray_row is 30 ms of which almost everything is the 1,957 shaded pixels, and
// moving further does not shade more pixels. The only budget they spend is the
// interval a traced frame stays reusable, and that trade is direct -- speed
// times interval is about one pixel of screen displacement. The table in
// tools/flower_stale.c is that price list.
#ifndef FLOWER_SWAY
#define FLOWER_SWAY .09f
#endif
#ifndef FLOWER_BREATH
#define FLOWER_BREATH .055f
#endif
#ifndef FLOWER_CUP
#define FLOWER_CUP .055f
#endif
// CRYSTAL is not a plant and does not sway; what it does is open and close.
// BEND is how far the petals swing over the breath, FLEX the per-petal offset
// that keeps them from moving as one piece.
#ifndef FLOWER_BEND
#define FLOWER_BEND .40f
#endif
#ifndef FLOWER_FLEX
#define FLOWER_FLEX .15f
#endif
static void cup(V root,float size,unsigned material,float yaw,float pitch) {
    // Six tepals in two whorls: upright, overlapping ellipsoidal surfaces.
    for(int i=0;i<6;i++) {
        float a=i*PI/3+.25f,r=((material==VIOLET?.39f:.27f)+FLOWER_CUP*sinf(elapsed*.8f))*size;
        V bottom=add(root,(V){.045f*size*cosf(a),0,.045f*size*sinf(a)});
        V top=add(root,(V){r*cosf(a),size*(.94f+(i%2)*.045f),r*sinf(a)});
        part(bottom,top,size*.20f,size*.075f,material,yaw,pitch);
    }
}
static void botanicals(float yaw,float pitch) {
    count=0;
    float sway=FLOWER_SWAY*sinf(elapsed*.7f),breath=FLOWER_BREATH*sinf(elapsed*.8f);
    V base={-.18f,-1.35f,0};
    if(current_species==FLOWER_VALLEY) {
        stem(base,(V){-.55f,.5f,0},(V){.05f,1.2f,0},9,.024f,yaw,pitch);
        part(base,(V){-.9f,.3f,-.08f},.17f,.035f,LEAF,yaw,pitch);
        part(base,(V){.72f,-.2f,-.12f},.16f,.035f,LEAF,yaw,pitch);
        for(int i=0;i<5;i++) {
            float t=.95f-i*.145f;V root=bezier(base,(V){-.55f,.5f,0},(V){.05f,1.2f,0},t);
            float side=i%2?-1:1;
            V top=add(root,(V){side*(.29f+i*.025f)+sway,-.12f,.08f});
            stem(root,add(root,(V){side*.3f,.1f,.04f}),top,3,.014f,yaw,pitch);
            bell(top,.32f+i*.018f,side*.14f+sway,yaw,pitch);
        }
    } else if(current_species==FLOWER_SUNFLOWER) {
        V center={.08f,.43f,.03f};
        stem(base,(V){.04f,-.4f,0},center,7,.038f,yaw,pitch);
        part((V){-.1f,-.86f,0},(V){-.75f,-.34f,-.03f},.17f,.045f,LEAF,yaw,pitch);
        part((V){-.04f,-.64f,0},(V){.68f,-.16f,-.04f},.16f,.04f,LEAF,yaw,pitch);
        for(int i=0;i<26;i++) {
            float a=i*2*PI/26+.03f*sinf(elapsed*.3f),r=.82f+(i%2)*.08f+breath;
            V a0=add(center,(V){cosf(a)*.29f,sinf(a)*.29f,-.07f});
            V a1=add(center,(V){cosf(a)*r,sinf(a)*r,-.04f+.08f*cosf(a*3+elapsed*.4f)});
            part(a0,a1,.077f,.035f,GOLD,yaw,pitch);
        }
        part(add(center,(V){-.36f,0,.045f}),add(center,(V){.36f,0,.045f}),.385f,.13f,SEED,yaw,pitch);
    } else if(current_species==FLOWER_SNOWDROP) {
        // A nodding head and three separated outer tepals make the silhouette
        // distinct from a radial flower, even on the 1.14-inch display.
        V top={.25f,.53f,.08f};
        stem(base,(V){-.65f,1.72f,0},(V){.16f,1.03f,0},9,.025f,yaw,pitch);
        stem((V){.16f,1.03f,0},(V){.4f,1.01f,.03f},top,4,.025f,yaw,pitch);
        part(base,(V){-.69f,.15f,0},.057f,.025f,LEAF,yaw,pitch);
        part(base,(V){.43f,-.04f,-.1f},.052f,.025f,LEAF,yaw,pitch);
        part(add(top,(V){0,.06f,0}),add(top,(V){0,-.15f,0}),.12f,.10f,LEAF,yaw,pitch);
        for(int i=0;i<3;i++) {
            float a=i*2*PI/3+.2f;
            V start=add(top,(V){cosf(a)*.04f,-.12f,sinf(a)*.04f});
            V tip=add(top,(V){cosf(a)*(.47f+breath),-.98f,sinf(a)*.28f});
            part(start,tip,.132f,.048f,IVORY,yaw,pitch);
        }
        part(add(top,(V){0,-.15f,.03f}),add(top,(V){0,-.66f,.03f}),.14f,.13f,INNER,yaw,pitch);
    } else if(current_species==FLOWER_TULIP) {
        V head={.1f,.22f,0};
        stem(base,(V){.1f,-.5f,0},head,7,.034f,yaw,pitch);
        part(base,(V){-.66f,.08f,-.1f},.14f,.03f,LEAF,yaw,pitch);
        part((V){-.08f,-1,0},(V){.69f,-.2f,-.06f},.13f,.03f,LEAF,yaw,pitch);
        cup(head,1,ROSE,yaw,pitch);
    } else if(current_species==FLOWER_DAFFODIL) {
        V head={.08f,.53f,0};
        stem(base,(V){-.2f,.1f,0},head,7,.026f,yaw,pitch);
        for(int i=0;i<3;i++)part(base,(V){-.6f+i*.48f,.13f+i*.09f,-.13f},.055f,.025f,LEAF,yaw,pitch);
        for(int i=0;i<6;i++) {
            float a=i*PI/3+.2f;
            part(add(head,(V){.09f*cosf(a),.09f*sinf(a),0}),
                 add(head,(V){.73f*cosf(a),.73f*sinf(a),-.08f}),.18f,.045f,IVORY,yaw,pitch);
        }
        trumpet(head,(V){.12f,-.4f,1},.5f,.225f,GOLD,1,yaw,pitch);
    } else if(current_species==FLOWER_CROCUS) {
        for(int i=0;i<3;i++) {
            V root={-.52f+i*.49f,-.66f+(i%2)*.27f,(i%2)*.12f};
            part((V){root.x,-1.3f,root.z},root,.019f,.018f,LEAF,yaw,pitch);
            cup(root,.83f,VIOLET,yaw,pitch);
            for(int j=0;j<3;j++)part(add(root,(V){(j-1)*.035f,.24f,.015f}),
                add(root,(V){(j-1)*.065f,.72f,.015f}),.023f,.018f,GOLD,yaw,pitch);
        }
        for(int i=0;i<6;i++)part((V){-.3f+i*.12f,-1.3f,-.15f},
            (V){-.88f+i*.34f,-.2f+(i%3)*.15f,-.12f},.026f,.016f,LEAF,yaw,pitch);
    } else if(current_species==FLOWER_CALLA) {
        // The calla was the one species `sway` never reached: its head is a
        // fixed point and everything above it hangs off that, so the wind blew
        // through it. Moving the head moves the stem's top, the spathe and the
        // spadix together, which is the same nod the others already have.
        V head={.04f+sway,.02f,0};
        stem(base,(V){0,-.55f,0},head,7,.03f,yaw,pitch);
        part(base,(V){-.75f,-.25f,-.15f},.18f,.035f,LEAF,yaw,pitch);
        part(base,(V){.69f,-.45f,-.12f},.18f,.035f,LEAF,yaw,pitch);
        trumpet(head,(V){.15f,1,-.1f},1.15f,.4f,IVORY,2,yaw,pitch);
        part(add(head,(V){.015f,.17f,.055f}),add(head,(V){.08f,.96f,.04f}),.052f,.05f,GOLD,yaw,pitch);
    }
}
// The menu row's entry point. Advances the rotation, then prepares whatever
// species the rotation currently holds.
void flower_prepare_rotating(float dt,int tilt_x,int tilt_y) {
    if(!isfinite(dt)||dt<0)dt=0;
    dt=fminf(dt,.1f);
    bloom_elapsed+=dt;
    float fade;
    if(bloom_elapsed>=FLOWER_ROTATE_S-FLOWER_FADE_HALF) {
        fade=(FLOWER_ROTATE_S-bloom_elapsed)/FLOWER_FADE_HALF;
        if(fade<=0) {
            // The darkest frame. Nothing of the old plant is on the strip, so
            // this is the one frame where replacing the part list is invisible.
            fade=0;
            bloom_species=bloom_next(bloom_species);
            bloom_elapsed=0;
        }
    }
    else if(bloom_elapsed<FLOWER_FADE_HALF) fade=bloom_elapsed/FLOWER_FADE_HALF;
    else fade=1;
    // After flower_prepare, which resets the dissolve: preparing one species
    // directly is what the tests and any future caller do, and that path has no
    // rotation to hide.
    flower_prepare(dt,tilt_x,tilt_y,bloom_species);
    // Advance vegetation only with the flower, without adding persistent state.
    if(seed_map)((GardenFrame*)(seed_map+32*32))->seed=bloom_rng;
    bloom_fade=fade>1?1:fade;
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
    current_species=species>=0&&species<FLOWER_SPECIES_COUNT?species:FLOWER_CRYSTAL;
    // Naming a species outright means drawing it, not dissolving into it. The
    // rotation sets its own factor after this returns.
    bloom_fade=1;
    bool rebuild;
    char *m=scene_mem(&flower_owner,FLOWER_BYTES,&rebuild);
    if(!m) { count=0;petals=NULL;depth=NULL;seed_map=NULL;return; }
    petals=(Petal*)m;
    depth=(float*)(m+sizeof(Petal)*MAX_PARTS);
    seed_map=(uint8_t*)(depth+FW);
    garden_prepare((GardenFrame*)(seed_map+32*32),elapsed);
    // The spiral and the bell profile are cached in that block, so they are as
    // new as it is. Keeping the flag outside and the data inside would be the
    // one way to get this wrong.
    if(rebuild)seeds_ready=false;
    prepare_seeds();
    float yaw=elapsed*.05f, pitch=.48f;
    count=PETALS;
    if(current_species!=FLOWER_CRYSTAL) {
        // Plants sway around their roots rather than rotating upside down.
        yaw=.035f*sinf(elapsed*.6f);pitch=.12f;
        botanicals(yaw,pitch);
    }
    for(unsigned i=0;i<count;i++) {
        Petal *p=&petals[i];
        if(current_species==FLOWER_CRYSTAL) {
        float a=i*2*PI/7, breath=.5f+.5f*sinf(elapsed*.65f);
        float bend=.15f+FLOWER_BEND*breath+FLOWER_FLEX*sinf(elapsed*.5f+i*.8f);
        V u={cosf(a)*cosf(bend),sinf(a)*cosf(bend),sinf(bend)};
        V v={-sinf(a),cosf(a),0};
        V n={-cosf(a)*sinf(bend),-sinf(a)*sinf(bend),cosf(bend)};
        p->c=rotate((V){cosf(a)*.57f,sinf(a)*.57f,.07f*sinf(a*2)},yaw,pitch);
        p->axis[0]=rotate(u,yaw,pitch);p->axis[1]=rotate(v,yaw,pitch);
        p->axis[2]=rotate(n,yaw,pitch);
        p->radius[0]=.66f+.08f*breath;p->radius[1]=.245f;p->radius[2]=.085f;
        if(i==7) {
            p->c=rotate((V){0,0,.13f},yaw,pitch);
            p->radius[0]=.20f;p->radius[1]=.20f;p->radius[2]=.16f;
        }
        p->material=i==7?HEART:PEARL;p->shape=0;
        }
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
        p->xmin=clampi((int)floorf(180+SCALE*(p->c.x-ex)),X0,W-1);
        p->xmax=clampi((int)ceilf(180+SCALE*(p->c.x+ex)),X0,W-1);
        p->ymin=clampi((int)floorf(65-SCALE*(p->c.y+ey)),12,119);
        p->ymax=clampi((int)ceilf(65-SCALE*(p->c.y-ey)),12,119);
    }
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
static uint16_t shade(V n,int petal,V hit) {
    unsigned material=petals[petal].material;
    bool inside=n.z<0;
    n=normal(n);
    if(inside)n=mul(n,-1);
    float diffuse=POS(dot(n,(V){-.36f,.48f,.8f}));
    float rim=1-POS(n.z);rim*=rim;
    float spec=POS(dot(n,(V){-.19f,.25f,.949f}));
    spec*=spec;spec*=spec;spec*=spec;spec*=spec;
    float band=POS(1-fabsf(n.x*.65f+n.y*.3f-.18f)*6);
    band=band*band*.22f;
    if(material>=LEAF) {
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
            if(p->shape) {g=220;b=47;light=.70f+.28f*diffuse;spec*=.3f;}
        }
        else if(material==ROSE) {r=242;g=47+35*longitudinal;b=104+32*longitudinal;}
        else if(material==VIOLET) {r=139+34*longitudinal;g=65+20*longitudinal;b=235;}
        else if(material==SEED) {
            int x=clampi((int)(16+15*longitudinal),0,31),y=clampi((int)(16+15*transverse),0,31);
            float seed=DIVR(seed_map[y*32+x],1.0f/255,255.0f);
            r=50+seed*72;g=25+seed*43;b=12+seed*16;spec*=.1f;
        } else {
            r=225;g=239;b=229;
            if(material==INNER&&longitudinal>.12f) {r=80;g=155;b=75;}
            spec*=.55f;
        }
        return rgb(r*light+spec*65+rim*16,g*light+spec*65+rim*20,b*light+spec*70+rim*23);
    }
    if(material==HEART)return rgb(35+diffuse*100+spec*115,66+diffuse*110+spec*70,61+diffuse*95+spec*90);
    return rgb(9+diffuse*22+rim*160+spec*160+band*150,
               30+diffuse*110+rim*45+spec*110+band*120,
               43+diffuse*104+rim*125+spec*140+band*150);
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
static bool bell_reject(const Petal *p,const float *o) {
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
#ifdef FLOWER_BELL_CHECK
    bool saw_root=false,saw_height=false,saw_depth=false,saw_clip=false;
#endif
    for(int band=0;band<LAT;band++) {
#ifdef FLOWER_NO_BAND_HOIST
        float lo=-1+2.0f*band/LAT,hi=-1+2.0f*(band+1)/LAT;
#else
        float lo=bell_lo[band],hi=bell_hi[band];
#endif
        float slope=bell_slopes[band],offset=bell_offsets[band];
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
            *best=z+p->c.z;*norm=n;found=true;
        }
    }
#ifdef FLOWER_BELL_CHECK
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
        float dy=DIVR(65-(y+.5f),INV_SCALE,SCALE)-p->c.y;
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
        for(int x=p->xmin;x<=p->xmax;x++) {
            float dx=DIVR(x+.5f-180,INV_SCALE,SCALE)-p->c.x;
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
void flower_draw(uint16_t *pixels,int y,int height) {
    if(!pixels||y<0||height<0||y>H||height>H-y)return;
    GardenFrame fallback;
    const GardenFrame *garden;
    if(seed_map)garden=(const GardenFrame*)(seed_map+32*32);
    else {garden_prepare(&fallback,elapsed);garden=&fallback;}
    for(int j=0;j<height;j++) {
        int py=y+j;uint16_t *row=pixels+j*W;
#ifdef ESP_PLATFORM
        PROF_FENCE;uint32_t t0=esp_cpu_get_cycle_count();PROF_FENCE;
        garden_row(row,py,garden);
        PROF_FENCE;uint32_t t1=esp_cpu_get_cycle_count();PROF_FENCE;
        prof_garden+=t1-t0;
#else
        garden_row(row,py,garden);
#endif
        // Without the block there is no flower, but there is still a sky. A
        // background that cannot allocate should look plain, not crash.
        if(py<12||py>119||!depth) {
#ifdef ESP_PLATFORM
            PROF_FENCE;prof_total+=esp_cpu_get_cycle_count()-t0;PROF_FENCE;
#endif
            continue;
        }
        for(int x=0;x<FW;x++)depth[x]=-1000;
        ray_row(row,py);
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
        // visits are the pixels the rejection arithmetic touches; hits are the
        // ones that reach sqrtf, the normal and shade(). The two have very
        // different unit costs -- roughly 20 cycles against 200 -- so which of
        // them dominates decides whether narrowing the spans is worth anything
        // at all, and nobody has ever counted the second one.
        // Inside ray_row: sqrt is every d>=0 (more than the hits), shade is
        // every hit. Two rsr.ccount either side of each is about 2 cycles on
        // regions of 150 and 1000, so the perturbation is under 2% -- but it
        // is not zero, and the sqrt figure carries the larger share of it.
        ESP_LOGI("garden","SPLIT frames=%u total=%.2f garden=%.2f pixels=%.2f decor=%.2f "
                 "ray=%.2f visits=%u hits=%u | sqrt=%.2f (%u calls, %u cy) shade=%.2f (%u cy) "
                 "bell=%.2f (%u visits, %u cy) | span=%.2f (%u rows, %u cy/visit) "
                 "bsqrt=%.2f (%u calls, %u cy) scan=%.2f pre=%.2f rest=%.2f "
                 "(ms/frame; total vs kernel= is the check)",
                 prof_frames,tot,gar,pix,gar-pix,tot-gar,
                 prof_visits/prof_frames,prof_hits/prof_frames,
                 prof_sqrt/240000.0/prof_frames,prof_sqrtn/prof_frames,
                 prof_sqrtn?prof_sqrt/prof_sqrtn:0,
                 prof_shade/240000.0/prof_frames,
                 prof_hits?prof_shade/prof_hits:0,
                 prof_bell/240000.0/prof_frames,prof_belln/prof_frames,
                 prof_belln?prof_bell/prof_belln:0,
                 // span holds sqrt, shade and bell; the per-visit figure has
                 // them taken back out, so it is the quadratic and the dx
                 // division and nothing else.
                 prof_span/240000.0/prof_frames,prof_spann/prof_frames,
                 prof_visits?(prof_span-prof_sqrt-prof_shade-prof_bell)/prof_visits:0,
                 prof_div/240000.0/prof_frames,prof_divn/prof_frames,
                 prof_divn?prof_div/prof_divn:0,
                 (prof_scan-prof_span)/240000.0/prof_frames,
                 prof_pre/240000.0/prof_frames,
                 tot-gar-(prof_scan+prof_pre)/240000.0/prof_frames);
        prof_total=prof_garden=prof_visits=prof_hits=0;prof_frames=0;
        prof_sqrt=prof_sqrtn=prof_shade=prof_bell=prof_belln=0;
        prof_span=prof_spann=prof_div=prof_divn=prof_scan=prof_pre=0;
    }
#endif
}
