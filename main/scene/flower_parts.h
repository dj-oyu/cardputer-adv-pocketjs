#pragma once
// The vocabulary a plant is written in.
//
// flower.c is a renderer: rays, a depth buffer, six conical bands and a strip.
// flower_species.c is a description of what a plant is. They met in one file
// for a long time, and the cost was that adding a snowdrop meant reading a ray
// tracer first. This header is everything the two genuinely share, and it is
// deliberately small: a part list, three globals and four vector helpers.
//
// Nothing that the per-pixel path depends on crosses this boundary. The
// reciprocals, slopes and band bounds that make bell_hit cheap are derived in
// flower.c and read in flower.c; the builders here only ever write c, axis[],
// radius[], material and shape, which is what a plant actually is.
#include "flower.h"
#include <math.h>

#define MAX_PARTS 56
#define FLOWER_BELL_BANDS 6
#define FLOWER_SHAPE_CLOCHE 3
// Species-owned profile coefficients; read-only Flash, no per-part storage.
extern const float flower_cloche_slopes[FLOWER_BELL_BANDS];
extern const float flower_cloche_offsets[FLOWER_BELL_BANDS];
#define PI 3.14159265358979323846f

typedef struct { float x,y,z; } V;
typedef struct {
    V c,axis[3];
    float radius[3],inv_radius[3],bd[3],oax[3],ba0,inv_a0,inv_d1,q[6],invzz;
    // The band quadratic's leading coefficient, its reciprocal, and the band
    // radius' slope along the ray -- all three per band. They look like they
    // belong to the pixel and do not: a = bd0^2 + bd2^2 - (slope*bd1)^2 names
    // only the part's own ray direction and the band profile, so it is fixed
    // for the whole frame. bell_hit was computing it, and dividing by it, once
    // per band on every one of ~2,000 visits. A float division on this part is
    // a call into a ROM routine that no disassembly search for `div` will show
    // (docs/pie-simd.md 3.7), which is how six of them per visit survived the
    // pass that hoisted the band bounds out of the same loop.
    float bell_dr[FLOWER_BELL_BANDS],bell_a[FLOWER_BELL_BANDS];
    float bell_inva[FLOWER_BELL_BANDS];
    // Half-extent in world units, on the two screen axes. Kept because the
    // screen boxes below cannot be computed until the camera is known, and the
    // camera is chosen by measuring the plant -- so the parts are walked twice
    // and this is what the first pass leaves for the second.
    float ex,ey;
    int xmin,xmax,ymin,ymax;
    unsigned material,shape;
} Petal;
enum { LEAF, IVORY, GOLD, SEED, INNER, ROSE, VIOLET, BLUE, RED, INK, CHECKER, HERB, FILAMENT, CORONA };

// The part list. The builders in flower_species.c fill it; flower.c derives
// the quadric, the reciprocals and the bounding box from what they leave, and
// then draws it. `count` is how many of MAX_PARTS are live.
extern Petal *petals;
extern unsigned count;
// Seconds of botanical time, wrapped at 120*PI. flower.c advances it once a
// frame; every builder reads it, and nothing else drives them -- which is what
// makes a pose reproducible and tools/flower_stale.c possible.
extern float elapsed;

static inline V add(V a,V b) { return (V){a.x+b.x,a.y+b.y,a.z+b.z}; }
static inline V mul(V a,float b) { return (V){a.x*b,a.y*b,a.z*b}; }
static inline float dot(V a,V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
// Measured at 1.16 ms of a 22 ms ray_row, so the reciprocal square root that
// would replace this sqrtf-then-divide is NOT worth writing. The idea is
// correct and keeps resurfacing -- software sqrt, two-stage call, a soft-float
// divide after it -- and it was wrong about the magnitude three times before
// the counter settled it. Left here so the next person finds the number
// instead of the reasoning.
static inline V normal(V a) { float d=dot(a,a);
    return mul(a,1.0f/sqrtf(d>1e-12f?d:1e-12f)); }

// The botanical builder leaves
// petals[0..count) filled for the species it was asked for.
void flower_build_botanicals(flower_species_t species,float yaw,float pitch);
