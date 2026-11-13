#ifndef DERBY_SER_INT_H
#define DERBY_SER_INT_H
/* DERBY WATCH's panning series in integers (docs/apps/derby-ser-division.md
 * sec.4, stage S1): pocket.derby.ser() without software doubles. Positions
 * are Q16 m, directions Q30, products int64, and the draws' inputs go to the
 * plans already in their Q formats (pocket_proc_draw_fixed). No QuickJS here:
 * pocket_derby.c binds it, and tools/games/test_derby_serq.c checks it
 * against the double series on the host (stage S0).
 *
 * The boundary for the PIE kernel (sec.6.3, P1). The chord ends' rotation into
 * the camera's frame and the chord culling (ou()) work on int16 lanes in a
 * struct of arrays: derby_ser_rotate_ref() and derby_ser_cull_ref() are the
 * scalar references, and a kernel that replaces them must give the very same
 * int16 outputs and keep-mask for every input (bit for bit; the scalar ones
 * are what the firmware runs until then). Everything they compute is defined
 * below in exact integer terms (floor shifts, no rounding left to the
 * compiler), so a QACC accumulate (EE.VMULAS.S16.QACC) followed by a 14-bit
 * EE.SRCMB.S16.QACC gives the same numbers. */
#include <stdbool.h>
#include <stdint.h>

#define DERBY_Q_ENDS 32u        /* chord ends at most (DERBY_ENDS_MAX) */
#define DERBY_Q_SLOTS 8u        /* series tables held at once */

/* ---- The PIE boundary: one lane per chord end, lanes rounded up to a
 * multiple of 8 (n), every array 16-byte aligned. The inputs (rx..tz) are
 * written by derby_ser_prepare(); the outputs (v0..v3n) by the rotation.
 * Lanes past the last end, and the ends whose difference from the camera is
 * past DERBY_Q_REL ("wide": the oval's two +-10 km ends), hold 0 inputs; the
 * caller ignores the cull's bits for the chords that touch a wide end and
 * tests those itself (derby_ser_cull_wide), so the kernel has no special
 * lanes. */
typedef struct {
    unsigned n;                          /* lanes, a multiple of 8, >= ends */
    int16_t *rx,*rz;                     /* in: end minus camera, Q3 m (|.| <= DERBY_Q_REL) */
    int16_t *tx,*tz;                     /* in: the course's unit tangent at the end, Q14 */
    int16_t *v0,*v1,*v2,*v3;             /* out: lateral, depth (Q3 m); the normal's lateral, depth (Q14) */
    int16_t *v0n,*v1n,*v2n,*v3n;         /* out: the same of the next end (lane j holds end j + 1) */
} derby_ends_soa;
#define DERBY_Q_REL 22000                /* |rx|, |rz| bound (2,750 m): rotated values stay within int16 */
#define DERBY_Q_W 1000                   /* |w| bound for culling (125 m, Q3) */
/* Bytes derby_ends_soa_init() needs for e lanes (plus 15 for the alignment). */
unsigned derby_ends_soa_bytes(unsigned ends);
/* Lays the arrays out in mem (any alignment; it aligns) for ends chord ends. */
void derby_ends_soa_init(derby_ends_soa *s,void *mem,unsigned ends);

/* The rotation by the view direction (cx, cz) = (cos, sin) of the camera's
 * heading, Q14, for every lane i < s->n:
 *   v0[i] = sat16(floor((rx*cz - rz*cx) / 2^14))      lateral (x pc3 - z pc2)
 *   v1[i] = sat16(floor((rx*cx + rz*cz) / 2^14))      depth   (x pc2 + z pc3)
 *   v2[i] = sat16(floor((-tz*cz - tx*cx) / 2^14))     the normal (-tz, tx) rotated
 *   v3[i] = sat16(floor((tx*cz - tz*cx) / 2^14))
 * then v*n[i] = v*[i + 1], and v*n[n - 1] = 0. Products are exact 32-bit
 * (|operand| <= 32767 x 16384); the sums fit 32 bits; sat16 clamps to
 * -32768..32767 and never acts for |rx|, |rz| <= DERBY_Q_REL and |cx|, |cz|,
 * |tx|, |tz| <= 16384 with cx^2 + cz^2 <= 2^28 (it is there for the kernel's
 * SRCMB, which saturates). */
void derby_ser_rotate_ref(derby_ends_soa *s,int16_t cx,int16_t cz);

/* One series' culling constants (conservative: a chord is culled only if the
 * double ou() of the same chord would cull it, sec.6.3; margin m + 3 m). */
typedef struct {
    int32_t w;      /* the series' offset, Q3 m, |w| <= DERBY_Q_W */
    int32_t f;      /* focal length, Q3 px (32..4095 px) */
    int32_t near;   /* floor(.02 f) - margin, Q3 m */
    int32_t far;    /* ceil(zf + margin), Q3 m */
    int32_t k;      /* margin * sqrt(f^2 + 160^2), rounded up, Q6 (Q3 m x Q3 px) */
} derby_cull_q;
/* The keep-mask: bit j (j < chords) set unless chord j (ends j and j + 1)
 * is wholly out on one plane. For each end (v0,v1,v2,v3 of lane j and the
 * v*n of lane j, all int32 arithmetic):
 *   d = v1 + floor(w*v3 / 2^14)          (= floor((v1*2^14 + w*v3) / 2^14))
 *   l = v0 + floor(w*v2 / 2^14)
 *   near: d < near      far: d > far
 *   left: 1280*d + l*f < -k             right: 1280*d - l*f < -k
 * (1280 = 160 Q3; sec.6.3's 160 d +- l f, both sides x 64). Out = the same
 * plane holds for both ends. |d|, |l| <= 32767 + |w|, so 1280 d + l f fits 31
 * bits. Bits >= chords are 0. chords <= 31. */
uint32_t derby_ser_cull_ref(const derby_ends_soa *s,unsigned chords,const derby_cull_q *p);

/* ---- The series in integers. */
/* The frame's camera (from aim()/view()'s doubles, once a frame). */
typedef struct {
    int32_t ex,ez;          /* position, Q16 m */
    int32_t dx,dz;          /* unit view direction, Q30 */
    int32_t f;              /* focal length, Q16 px, 32..4095 px */
    int32_t invf;           /* 1/f, Q32 */
    int32_t zf;             /* far depth, Q16 m */
    int32_t zn;             /* near depth f/50, Q16 m */
    int32_t fr3;            /* sqrt(f^2 + 160^2) rounded up, Q3 px (the cull's margin scale) */
    int16_t cx,cz;          /* dx, dz in Q14 (the rotation's) */
} derby_cam_q;
/* One chord of one series (w, s, g0), frame independent: its ends on the s
 * grid from g0 (ga, gb, Q16 m), the world point A = pose(ga, w) (Q16), the
 * step t = (B - A)/(gb - ga) (Q30 a m) and the unit normal (Q30, the turf's
 * width). skip: ga == gb or out of range (the double series draws nothing
 * for ga == gb either: dx = 0/0). */
typedef struct {
    int32_t ga,gb,ax,az,tx,tz,nx,nz;
} derby_chord_q;
#define DERBY_CHORD_SKIP INT32_MIN   /* in tx */
/* What a ser() call draws with. h, hl, crowd: plan handles (0: none). */
typedef struct {
    int32_t s,g0;           /* Q16 m; s >= 0.25 m, <= 64 m */
    int32_t L;              /* LOD divisor, >= 1 */
    int32_t a,b;            /* rails: colour (int), top (Q16); turf: width (Q16) */
    bool turf;
    int32_t hl_lines,cells; /* the stands' hl line count, cells a m (Q16) */
    uint32_t h,hl,crowd;
} derby_series_q;
/* Counters (host checks and r5sim; the firmware keeps sat and bad only). */
typedef struct {
    uint32_t chords,drawn,draws;     /* chords computed, with a draw, draws */
    uint32_t sat;                    /* inputs clamped to int32 (must stay 0) */
    uint32_t bad;                    /* draws skipped: depth <= 0 at the far end (must stay 0) */
#ifdef DERBY_SER_CHECK
    /* Invariants (sec.5.2): Newton's start r against 1/z (relative 2^-12
     * plus one LSB), its step |dz|/z at the near end (<= .31, coarse levels),
     * the line count 1..255, and two chords' shared end on the screen within
     * 1 px (joins checked / broken), worst px. */
    uint32_t newton_start,newton_step,count_range,joins,join_bad;
    double join_worst;
    int32_t last_j,last_hi_end;      /* the previous chord in this call, its far end (-1: none) */
    double last_x,last_y;
#endif
#ifdef DERBY_SER_COUNT
    uint64_t mul64,div64,div32,isqrt;
#endif
} derby_ser_stats;
/* The draw callback: one draw's 8 inputs in the plan's Q formats; nonzero
 * stops the call (an exception, or the course changed under it). */
typedef int (*derby_emit_fn)(void *user,uint32_t handle,const int32_t in[8]);
/* The Q formats derby_ser_chord_q() writes, by plan (prail, pt, hl, crowd);
 * the caller checks a plan's input_q against them before an integer call. */
extern const uint8_t derby_q_prail[8],derby_q_pt[8],derby_q_hl[8],derby_q_crowd[8];
/* Chord j of the series on camera c: clip to the screen's four planes, LOD,
 * draws (as the double ser()'s body for one chord, L >= 1). join_j: chord j's
 * index (the join check only). Returns the emit's nonzero, else 0. */
int derby_ser_chord_q(const derby_cam_q *c,const derby_series_q *p,const derby_chord_q *ch,unsigned j,
                      derby_emit_fn emit,void *user,derby_ser_stats *st);
/* The wide chords' cull (an end beyond DERBY_Q_REL), scalar in int64 Q16:
 * true if wholly out by margin_q16 + 1 m on one plane. pa, pb: the chord's
 * ends (world, Q16), ta, tb: the course's unit tangents there (Q30; the
 * normal is (-tz, tx)), w: offset (Q16). */
bool derby_ser_cull_wide(const derby_cam_q *c,const int32_t pa[2],const int32_t pb[2],const int32_t ta[2],
                         const int32_t tb[2],int32_t w,int32_t margin_q16);
/* Fills one series' chord table: pose(g, w, out[4]) is the course's double
 * pose (x, z, tangent x, z); ends: the chord ends CH (Q16). False if a value
 * is out of range (then the series stays on the double path). */
typedef void (*derby_pose_fn)(void *user,double g,double w,double out[4]);
bool derby_ser_table(derby_chord_q *rows,const int32_t *ends,unsigned n_ends,int32_t w,int32_t s,int32_t g0,
                     derby_pose_fn pose,void *user);
/* Culling constants for margin m (Q16 m) at offset w (Q16 m). False when |w|
 * is past DERBY_Q_W (then nothing is culled). */
bool derby_ser_cull_params(const derby_cam_q *c,int32_t w,int32_t m,derby_cull_q *out);
/* The rotation's inputs for the frame: end positions (Q16 world) and unit
 * tangents (Q14) of n_ends ends into s; returns the mask of ends whose
 * difference is past DERBY_Q_REL (their lanes hold 0 inputs; their
 * chords go through derby_ser_cull_wide()). */
uint32_t derby_ser_prepare(derby_ends_soa *s,const derby_cam_q *c,const int32_t (*pos)[2],const int16_t (*tan14)[2],
                           unsigned n_ends);
#endif
