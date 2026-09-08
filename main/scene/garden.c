#include "garden.h"
#include <math.h>
#include <stdlib.h>
#ifdef ESP_PLATFORM
#include "esp_cpu.h"
#if GARDEN_MOTE_ONLY
#include "esp_log.h"
#endif
// TEMPORARY (see garden.h). One rsr.ccount either side of the vector half,
// twice a row; the fences stop the compiler moving work across them.
#define GARDEN_FENCE __asm__ __volatile__("":::"memory")
static uint32_t garden_pixel_cycles;
uint32_t garden_prof_pixels(void) {
    uint32_t v=garden_pixel_cycles;garden_pixel_cycles=0;return v;
}
#endif

// No writable statics, LUTs, images or vertex lists. A broad warm scattering
// lobe sits in cool mist, broken by drifting density and canopy silhouettes.
//
// One exception, and it is deliberate rather than a lapse: garden_pixel_cycles
// below is four bytes of writable DIRAM under ESP_PLATFORM. It is a profiling
// counter, not scene state -- nothing drawn depends on it, the host build does
// not have it, and it goes when flower.c's TEMPORARY counters go. The claim
// above is about what the scene needs to draw itself, which is still nothing.
static unsigned garden_hash(unsigned v) {
    v^=v>>16;v*=0x7feb352du;v^=v>>15;v*=0x846ca68bu;return v^(v>>16);
}
// Q8 smoothstep/value noise. Lattice values come from hashes, never a LUT.
static int garden_smooth(int t) { return t*t*(768-2*t)/65536; }
static int garden_noise(unsigned p,unsigned seed) {
    unsigned c=(p>>8)&255;
    int a=(int)(garden_hash(c+seed)&255);
    int b=(int)(garden_hash(((c+1)&255)+seed)&255);
    return a+(b-a)*garden_smooth((int)(p&255))/256;
}
// A four-second lattice spacing for wind/warping; same 128-second period as
// advection. Adjacent plants sample nearby positions in this shared field.
static int garden_motion(unsigned p,unsigned seed) {
    unsigned c=(p>>11)&31;
    int a=(int)(garden_hash(c+seed)&255);
    int b=(int)(garden_hash(((c+1)&31)+seed)&255);
    return a+(b-a)*garden_smooth((int)((p>>3)&255))/256;
}
typedef struct { int cell,left,right; } GardenSpan;
// The scalar field, kept because garden_row_scalar below is the only readable
// statement of what the lane code computes.
static int __attribute__((unused))
garden_density(GardenSpan *s,int px,int y,int shift) {
    int c=px>>(shift+8),mask=(256>>shift)-1;
    if(c!=s->cell) {
        s->cell=c;unsigned cy=(unsigned)(y>>shift);
        int fy=garden_smooth((y&((1<<shift)-1))*256/(1<<shift));
        int a=(int)(garden_hash((c&mask)+cy*19)&255);
        int b=(int)(garden_hash((c&mask)+(cy+1)*19)&255);
        int aa=(int)(garden_hash(((c+1)&mask)+cy*19)&255);
        int bb=(int)(garden_hash(((c+1)&mask)+(cy+1)*19)&255);
        s->left=a+(b-a)*fy/256;s->right=aa+(bb-aa)*fy/256;
    }
    int fx=garden_smooth((px>>shift)&255);
    return s->left+(s->right-s->left)*fx/256;
}
// A cell's corner, which is all garden_density ever reads out of the lattice:
// its `left` for cell c and its `right` for cell c+1 are the same function, so
// one array of (mask+1) corners per row describes the whole octave.
static int garden_corner(int c,int mask,int y,int shift) {
    unsigned cy=(unsigned)(y>>shift);
    // *256/(1<<shift) is <<(8-shift), and `shift` is a parameter so the
    // compiler could not see that: it emitted a real hardware divide, twelve
    // times a row. Exact, not an approximation -- the divisor is a power of two
    // and the numerator is non-negative.
    int fy=garden_smooth((y&((1<<shift)-1))<<(8-shift));
    int a=(int)(garden_hash((unsigned)(c&mask)+cy*19)&255);
    int b=(int)(garden_hash((unsigned)(c&mask)+(cy+1)*19)&255);
    return a+(b-a)*fy/256;
}
// Stable one-bit spatial dither softens RGB565 steps without flicker. It is a
// function rather than an expression because tools/test_garden.c holds it to a
// contract -- uniform over the four values, uncorrelated with either neighbour
// -- and a test can only hold what it can call. Whatever hash this uses has to
// keep passing that, which is what stops "the noise does not matter" from
// becoming "nobody checked the noise".
//
// This is the second hash to hold that contract. The 32-bit one it replaces
// cannot be done in 16-bit lanes, so it was searched for among the shapes that
// can: one multiply, one XOR against a per-row scalar, and two squarings whose
// full 32-bit products come back out of QACC shifted. The XOR is not
// decoration -- EE.VADDS.S16 saturates, so there is no modular add in the
// instruction set and the row term cannot be added. Constants were chosen by
// sweeping 40,000 quadruples against the statistics below rather than picked;
// the obvious shapes fail loudly (a single multiply is linear in x, so
// neighbours differ by a constant and P(same as left) collapses to 0, and one
// squaring alone leaves a 7% imbalance between the four values).
// The shifts are 17 and 16 rather than the 8 that would mix best, because
// EE.SRCMB.S16.QACC *saturates* on its way out of the accumulator. h*h reaches
// 2^32, and taken out at a shift of 8 every large lane would come back as
// 32767 -- the same value -- and the dither would collapse. 17 is the smallest
// shift that keeps the square inside a signed lane, and the second product is
// taken at 16 for the same reason. Nothing else in these kernels saturates;
// this is the one place the bound is tight, and it is what the sweep in
// tools/pie/models/garden_model.c is checking when it prints the four counts.
#define GARDEN_DKX 18453
#define GARDEN_DKY 26253
#define GARDEN_DKC 17872
#define GARDEN_DKM 42589
static int garden_dither(int x,int y) {
    unsigned h=((unsigned)(x*GARDEN_DKX)^(unsigned)(y*GARDEN_DKY+GARDEN_DKC))&0xffffu;
    unsigned s=(h*h)>>17;
    return (int)(((s*GARDEN_DKM)>>16)&3u);
}
static uint16_t garden_rgb(int r,int g,int b) {
    return (uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3));
}
static uint16_t garden_mix(uint16_t a,uint16_t b,unsigned f) {
    unsigned r=(((a>>11)&31)*(256-f)+((b>>11)&31)*f)>>8;
    unsigned g=(((a>>5)&63)*(256-f)+((b>>5)&63)*f)>>8;
    unsigned blue=((a&31)*(256-f)+(b&31)*f)>>8;
    return (uint16_t)(r<<11|g<<5|blue);
}
// ---------------------------------------------------------------------------
// The pixel loop, in eight-pixel lanes.
//
// Three passes over the row replace one scalar loop: two that lay the noise
// field down in `dens` and one that turns it into pixels. This shape is forced
// by what PIE cannot do rather than chosen. The lattice corners change part way
// through a block, and the only way to hold two cells' corners in lanes at once
// is a compare and a mask; doing that inline would need six live vectors that
// the pixel pass does not have to spare, on a machine with eight. Split out, no
// pass needs more than eight, no value spills, and the store pointer stays
// 16-byte aligned across the whole row -- which matters because a 128-bit store
// to a misaligned address does not fault, it silently rounds down (TRM 1.7, and
// docs/pie-simd.md 5).
//
// The row's constants -- forty in the pixel pass, seven or eight in each
// octave -- never take a register at all. They are broadcast once into a table
// that the loop walks in issue order, and the pass reads each one where it is
// used. In the octaves that walk is free: docs/pie-simd.md:70-76 swept the
// count of fused loads from 0 to 32 per block and measured the coefficient at
// zero, so every constant there rides an arithmetic instruction that was
// happening anyway. The pixel pass has 28 instructions with a .LD.INCP form
// against 40 constants, so it pays a plain load for each instead; fusing what
// can be fused would take its block from 135 instructions to about 107 and is
// deliberately left for after the device has spoken.
//
// Holding constants in registers instead was never an option: there is no
// instruction that distributes one element of a vector register to all lanes
// (docs/pie-simd.md line 20), so a constant either comes from memory or is
// built. What the table buys is the eight registers -- only the x ramp, the
// ambient, the haze and the accumulating sunlight live across more than a few
// instructions, and nothing spills.
//
// Every division in the scalar loop becomes a multiply by a rounded-up
// reciprocal, proven over its whole domain in tools/pie/models/garden_model.c.
// The rounding is up rather than nearest on purpose: it makes the lobe exactly
// zero at its own clamped edge, which is what lets the kernel drop the
// out-of-range test the scalar code needs and clamp instead.
#define GARDEN_M48 1366                 /* ceil(2^16/48): density/12 undivided */
#define GARDEN_M80 820                  /* ceil(2^16/80): density/20 undivided */
#define GARDEN_M3  21846                /* ceil(2^16/3)  */
typedef struct {
    int center,width,mww;
    int at0,w0,mw0,at1,w1,mw1;
    int ambient_y,kbase,dy;
    int p5,p6;
    int vc[4],vf[8];
} GardenRow;
static int garden_recip(int d,int sh) { return (int)((((long)1<<sh)+d-1)/d); }

// One octave of the field across the row. `step` is how far the lattice
// coordinate moves per pixel -- 4 for the 64-pixel octave, 8 for the 32-pixel
// one -- and `v` holds that octave's (mask+1) corners for this row.
//
// The row is cut into runs of blocks that start in the same cell. A run never
// splits a block: the block straddling a cell edge stays in the run it began
// in, and the lanes past the edge are moved to the next cell by ANDing the
// corner deltas with EE.VCMP.LT.S16's mask. That is the whole reason the
// destination stays aligned; cutting at the true edge instead would start most
// runs at an odd pixel.
static void __attribute__((unused))
garden_octave_lanes(int16_t *dens,int n,int rc0,int step,
                                int a,int da,int ddb,int first) {
    for(int i=0;i<n;i++)for(int l=0;l<8;l++) {
        int rc=rc0+step*(i*8+l);
        // t*t peaks at 65025 and stays inside a lane; the cubic's product does
        // not, so it comes back out of QACC shifted by 16.
        int t=rc&255,w=768-2*t,fx=((t*t)*w)>>16;
        int nm=rc>255?-1:0;
        int lo=a+(da&nm),qd=da+(ddb&nm),hi=lo+qd;
        // a+(b-a)*f/256 rewritten so both terms are non-negative and one QACC
        // pass carries them; the two differ by at most one step, and only
        // where C truncates a negative product toward zero.
        int d=(lo*(256-fx)+hi*fx)>>8;
        int x=i*8+l;
        // Undivided on purpose: dens holds 3Dc+Df, and the pixel pass folds
        // the quarter into its own reciprocals.
        dens[x]=(int16_t)(first?3*d:dens[x]+d);
    }
}
// The canopy's span, written as lanes.
//
// It is 8,299 of the 16,218 blends garden_row's scalar decorations make in a
// frame -- more than the trunks and the grass together -- and it makes them in
// 154 runs of about 54 contiguous pixels, one per ellipse-row, which is the
// shape a vector unit wants. The trunks are 405 spans of fifteen and the grass
// is spans of four; neither is worth eight lanes.
//
// Three things had to be true for this to be expressible without moving a
// pixel, and all three are swept in tools/pie/models/garden_model.c:
//
//   mrr = ceil(2^26/rr) reaches 85,598 and does not fit a 16-bit lane. Split it
//   as hi*256+lo and take dx2*hi*256 as (dx2*16)*(hi*16), which does fit,
//   because dx2 <= 1936 and hi <= 334.
//
//   q*3/5 is (q*39322)>>16 exactly over q in 0..256.
//
//   The `if(q>0)` disappears rather than becoming a mask: clamping q to zero
//   gives f=0, and (A*256 + B*0)>>8 is A. Blending with zero alpha is the
//   identity, so the branch is free to go.
static void garden_canopy_row(uint16_t *row,int lo,int hi,int cx,int mrr,int qy,
                              uint16_t leafy) {
    int mhi=(mrr>>8)*16,mlo=mrr&255,qbase=256-qy;
    int lr=(leafy>>11)&31,lg=(leafy>>5)&63,lb=leafy&31;
    for(int x=lo;x<=hi;x++) {
        int dx=x-cx,dx2=dx*dx;
        int t=((dx2*16)*mhi+dx2*mlo)>>18;
        int q=qbase-t;
        if(q<0)q=0;
        int f=(q*39322)>>16,g=256-f;
        unsigned a=row[x];
        row[x]=(uint16_t)(((((a>>11)&31)*g+lr*f)>>8)*2048
                         +((((a>>5)&63)*g+lg*f)>>8)*32
                         +(((a&31)*g+lb*f)>>8));
    }
}
// One sunlight shoulder. The clamp replaces the scalar `if`: at |d| == w the
// rounded-up reciprocal makes s exactly 0, so an out-of-band column adds
// nothing without being tested. s can be 256, so s*s leaves a lane -- it is
// regrouped as (s*gain)*s, which does not.
static int garden_shoulder(int x,int at,int w,int m,int gain,int haze4) {
    int d=x-at;
    if(d<-w)d=-w;
    if(d>w)d=w;
    int s=256-((((d*d)&0xffff)*m)>>14);
    int t=(((s*gain)&0xffff)*s)>>8;
    return (t*haze4)>>18;
}
// The pixel pass. Everything here is a lane operation: the C is the statement
// of what the assembly does, in the order it does it.
// One pixel of the light, with `extra` added to the sunlight before the
// channels are formed. It exists so that the mote touch-up and the pixel pass
// are the same arithmetic rather than two copies of it -- the approximations in
// here (the combined shifts, the two-step shoulder) are exactly the ones the
// PIE kernel makes, and a second copy would drift away from them silently.
static inline uint16_t garden_shade_pixel(int x,int dn,const GardenRow *r,int extra) {
    int u=x-r->center;
    if(u<-r->width)u=-r->width;
    if(u>r->width)u=r->width;
    int q=256-((((u*u)&0xffff)*r->mww)>>14);
    int ambient=r->ambient_y+((dn*GARDEN_M48)>>16);
    int k=r->kbase+((dn*GARDEN_M80)>>16),haze=320+dn;
    int sun=(((q*k)&0xffff)*q)>>16;
    sun+=garden_shoulder(x,r->at0,r->w0,r->mw0,4,haze);
    sun+=garden_shoulder(x,r->at1,r->w1,r->mw1,7,haze);
    unsigned h=((unsigned)(x*GARDEN_DKX)^(unsigned)r->dy)&0xffffu;
    int d=(int)((((h*h)>>17)*GARDEN_DKM>>16)&3u);
    int fr=10,fg=22,fb=28;      /* the channel floors, named so the switch can drop them */
#if GARDEN_MOTE_ONLY
    // Everything the BACKGROUND writes, dropped -- and nothing else. See the
    // switch in garden.h: `q` is computed above and survives untouched, because
    // the mote is gated by it and zeroing the shaft by zeroing its lobe would
    // take the motes with it and prove nothing. The line below is the whole of
    // the blanking, and it is placed after q and before the mote's term on
    // purpose. Do not move it up.
    ambient=0;sun=0;d=0;fr=fg=fb=0;
#endif
    // The mote's own light, gated by the shaft it sits in. Adding it flat would
    // make a mote just as bright over the woodland as inside the beam; scaling
    // by q means it only exists where the light does, which is both what dust
    // does and what the effect is for. q is already here, so it is one multiply.
    sun+=(extra*q)>>8;
    int cr=((ambient+5*sun)>>1)+d+fr;
    int cg=((3*ambient+6*sun)>>2)+d+fg;
    int cb=(ambient+((sun*GARDEN_M3)>>16)+d+fb)>>3;
    return (uint16_t)((((cr*256)&0xF800)|((cg*8)&0x07E0))|cb);
}
static void __attribute__((unused))
garden_pixels(uint16_t *row,const int16_t *dens,const GardenRow *r) {
    for(int x=0;x<240;x++)row[x]=garden_shade_pixel(x,dens[x],r,0);
}
// Where the light is, at this row. The one definition of it.
//
// Two places need it and they must not disagree. The renderer treats a column
// as lit when |x-center| < half, because that is exactly where the lobe
// q = 256 - u*u*256/(half*half) is positive; garden_motes uses the same test to
// decide whether a particle is still alive. If those two drifted apart, midges
// would either die a few pixels inside the visible edge -- a band where the
// swarm mysteriously thins -- or be tracked a few pixels outside it, which is
// merely wasteful. The warp term is per-row and comes from the phase, so a
// version of this that left it out would be wrong in the first way.
static void garden_shaft(int y,const GardenFrame *f,int *center,int *half) {
    *center=200-(y+40)*3/4+f->sun+(garden_motion((unsigned)(f->phase+y*64),419)-128)/16;
    *half=58+y/3;
}
// The only place a mote's heading is written after it is seeded.
//
// The dying latch lives in the sign of that byte, so an assignment made without
// thinking anywhere else would silently resurrect a dying particle -- and the
// symptom is subtle enough to go unnoticed for a long time. That is the same
// shape as petal_reciprocals being forgettable in flower.c, and it gets the
// same fix: one greppable function, and the invariant holds because there is
// one place rather than because anybody remembered.
static void garden_mote_turn(GardenMote *m,int turn) {
    int dying=m->dir<0,h=(dying?-m->dir:m->dir)-1;   /* 0..126 */
    h+=turn;
    if(h<0)h+=127;else if(h>=127)h-=127;
    m->dir=(int8_t)(dying?-(h+1):h+1);
}
// The heading in 256ths of a turn, reversed while dying -- which is the whole
// of the outward nudge, and costs nothing because it is the latch.
static int garden_mote_dir(const GardenMote *m) {
    int h=((m->dir<0?-m->dir:m->dir)-1)*2;
    return m->dir<0?h+128:h;
}
// A whole turn in 256, to about 5%: the parabola the ocean kernel uses, which
// is plenty for something nobody measures the trajectory of, and keeps the
// swarm out of libm. Derived rather than tabulated, so the file's opening claim
// about LUTs stays true -- a 64-entry quarter table would have been smaller to
// read and a promise to break.
static int garden_sin(unsigned a) {
    int u=(int)(a&255)-128,v=u<0?-u:u;
    return (u*(128-v))>>4;                  /* -256..256 */
}
// The swarm, advanced one frame.
//
// Everything random here is a function of (phase, particle), so the same
// sequence of times produces the same swarm -- the positions integrate, but the
// draws that move them do not carry anything. That is what makes a swarm
// testable at all.
static void garden_motes(GardenFrame *f) {
    for(int i=0;i<GARDEN_MOTES;i++) {
        GardenMote *m=&f->mote[i];
        unsigned h=garden_hash((unsigned)f->phase*2654435761u
                               +(unsigned)i*0x9E3779B9u+f->seed);
        // Move first, then cull, so that when this returns every particle is
        // inside the shaft. Culling first would leave one outside for the frame
        // it dies on -- harmless, because the gate by q makes it invisible
        // there, but it would mean the invariant is only nearly true, and a
        // nearly-true invariant is not one anything can be checked against.
        if(m->speed) {
            // Heading, not position. A small turn most frames and a sharp one
            // about one frame in eight is what makes it dart rather than
            // wander; random-walking the position gives noise instead.
            int turn=(int)(h&15)-8;
            if(((h>>8)&7)==0)turn=(int)((h>>16)&63)-32;
            garden_mote_turn(m,turn);
            // Speed, re-drawn occasionally: mostly a cruise, sometimes a hover
            // of very nearly nothing. The pauses are most of what reads as
            // alive. Never zero -- zero is the marker for a particle that has
            // never been seeded, and a hover that drew it used to kill one
            // every sixty-fourth particle-frame.
            if(((h>>12)&15)==0)m->speed=(uint8_t)(1+((h>>26)&3));
            else if(((h>>12)&3)==0)m->speed=(uint8_t)(10+((h>>24)&11));
            // Vertical travel is scaled up by half: they bob more than they
            // wander sideways.
            int a=garden_mote_dir(m);
            int dx=(garden_sin((unsigned)a+64u)*m->speed)>>8;
            int dy=(garden_sin((unsigned)a)*m->speed*3)>>9;
            // The tether, and its gain is the whole of whether this reads as a
            // swarm or as fourteen things leaving. A step of about a pixel with
            // a heading that persists some eight frames is a random walk of ~9
            // px per correlation time, so holding a volume of about thirty
            // needs a pull near a sixteenth; at 1/128 they reached 113 px from
            // home, which is most of the screen. Held twice as firmly in y,
            // because the vertical step is half again the horizontal one and
            // the bob would otherwise become a drift out of the frame.
            int hc,hh;garden_shaft(m->hy>>4,f,&hc,&hh);
            int hx=(hc+m->hoff)*16;
            m->x=(int16_t)(m->x+dx+((hx-m->x)>>4));
            m->y=(int16_t)(m->y+dy+((m->hy-m->y)>>3));
            if(m->age<255)m->age++;
            // The latch. Set once, never cleared: coming back above the line
            // does not cancel it, which is what stops a particle sitting on the
            // line from flickering between fading and not.
            if(m->dir>0&&(m->y>>4)>GARDEN_DOOM) {
                m->dir=(int8_t)-m->dir;
                m->dim=GARDEN_DYING;
            } else if(m->dir<0&&m->dim) m->dim--;
        }
        int y=m->y>>4,center,half;
        int alive=0;
        if(m->speed&&m->dir&&!(m->dir<0&&!m->dim)&&y>=0&&y<135) {
            garden_shaft(y,f,&center,&half);
            int u=(m->x>>4)-center;
            alive=u>-half&&u<half;
        }
        if(alive)continue;
        // Replaced where it can be seen. This is also how the swarm starts: a
        // zeroed GardenFrame is fourteen particles at (0,0), which is outside
        // the shaft, so the cull is the seeding rule as well as the death rule
        // and there is no separate initialisation to forget.
        unsigned g=garden_hash(h^0xA5A5u);
        int ny=25+(int)(g%88u);
        garden_shaft(ny,f,&center,&half);
        // Biased to the axis by the square: more of them where the light is
        // strong, which is where the real thing gathers.
        int off=(int)((g>>8)&127)-64;
        off=off*(off<0?-off:off)/64;
        m->hoff=(int16_t)(off*half/90);
        m->hy=(int16_t)(ny*16);
        m->x=(int16_t)((center+m->hoff)*16);m->y=m->hy;
        m->dir=(int8_t)(1+(int)((g>>16)&126));      /* 1..127: alive, never zero */
        m->speed=(uint8_t)(10+((g>>24)&11));
        m->glow=(uint8_t)(GARDEN_GLOW_BASE+((g>>20)&31));
        m->age=0;m->dim=0;
    }
}
#if GARDEN_MOTE_INDEX && !GARDEN_NO_MOTES
// The scan, hoisted out of the row loop. Written here and nowhere else, and
// read-only from garden_row -- which is what makes it legal at all, given that
// garden_row takes a const GardenFrame *.
//
// The membership rule has to be the same one the row uses, so it is stated in
// the same terms: a mote lies across rows top and top+1, and the second gets a
// share of zero when the position is exactly on a row boundary. Get this wrong
// in the generous direction and the index costs a little; wrong in the mean
// direction and a mote flickers.
static void garden_mote_index(GardenFrame *f) {
    for(int y=0;y<GARDEN_ROWS;y++)f->rowmask[y]=0;
    for(int i=0;i<GARDEN_MOTES;i++) {
        const GardenMote *m=&f->mote[i];
        if(!m->glow)continue;
        int top=m->y>>4,frac=m->y&15;
        if(top>=0&&top<GARDEN_ROWS)f->rowmask[top]|=(uint16_t)(1u<<i);
        if(frac&&top+1>=0&&top+1<GARDEN_ROWS)f->rowmask[top+1]|=(uint16_t)(1u<<i);
    }
}
#endif
void garden_prepare(GardenFrame *f,float time) {
    // Fractional advection avoids whole-pixel jumps. All noise is periodic at
    // this wrap, including wind, so long-running animation has no reset seam.
    f->phase=(int)(fmodf(fmaxf(time,0),128.0f)*512);
    f->sun=(garden_motion((unsigned)f->phase,83)-128)/24;
    f->breath=(garden_motion((unsigned)f->phase,193)-128)/24;
    f->seed=0;
#if GARDEN_NO_MOTES
    // The whole feature, gone: no motion, no index, no touch-up. Nothing else
    // in the frame changes, which is the entire point -- see garden.h.
    (void)garden_motes;
#else
    garden_motes(f);
#if GARDEN_MOTE_INDEX
    garden_mote_index(f);
#endif
#endif
#if GARDEN_MOTE_ONLY && !GARDEN_NO_MOTES && defined(ESP_PLATFORM)
    // Where they are, once a second, so a capture can be checked against
    // arithmetic instead of against an impression. Positions in whole pixels
    // and the three things that decide whether anything is written: glow, the
    // birth ramp, and the dying fade. A writable static, which the rest of this
    // file does not permit -- it is here only in the diagnostic build.
    {
        static unsigned tick;
        if(tick++%25u==0)
            for(int i=0;i<GARDEN_MOTES;i++) {
                const GardenMote *m=&f->mote[i];
                ESP_LOGI("garden","MOTE %d x=%d y=%d frac=%d glow=%u age=%u dim=%u dir=%d",
                         i,m->x>>4,m->y>>4,m->y&15,m->glow,m->age,m->dim,m->dir);
            }
    }
#endif
}
// The pixel loop the three lane passes above replace, kept because it is the
// only readable statement of what they compute. It is not called: anything
// that changes here has to change there, and tools/test_garden.c holds the
// pair to a contract -- same per-channel range and mean, same dither
// statistics -- rather than to bit equality, which the reciprocals give up.
static void __attribute__((unused))
garden_row_scalar(uint16_t *row,int y,const GardenFrame *f) {
    int center=200-(y+40)*3/4+f->sun,width=58+y/3;
    center+=(garden_motion((unsigned)(f->phase+y*64),419)-128)/16;
    int ambient_y=20+y/15,ww=width*width;
    int at0=center-18,w0=10+y/13,ww0=w0*w0;
    int at1=center+27,w1=10+y/8,ww1=w1*w1;
    GardenSpan fine={-1,0,0},coarse={-1,0,0};
    for(int x=0;x<240;x++) {
        // Two octaves, 64/32 px; cache lattice corners on the stack per row.
        // The broad field dominates, retaining directional shafts, not smoke.
        int px=x*256+f->phase;
        int density=(3*garden_density(&coarse,px,y,6)
                       +garden_density(&fine,px,y,5))/4;
        int ambient=ambient_y+density/12;
        // |u| >= width makes q non-positive, so the division is skipped rather
        // than performed and thrown away; the same holds for both shoulders.
        int u=x-center,q=(u>-width&&u<width)?256-u*u*256/ww:0;
        int sun=q>0?q*q*(17+f->breath+density/20)/65536:0,haze=80+density;
        int d0=x-at0;
        if(d0>-w0&&d0<w0) {
            int shoulder=256-d0*d0*256/ww0;
            if(shoulder>0)sun+=shoulder*shoulder*4*haze/(65536*256);
        }
        int d1=x-at1;
        if(d1>-w1&&d1<w1) {
            int shoulder=256-d1*d1*256/ww1;
            if(shoulder>0)sun+=shoulder*shoulder*7*haze/(65536*256);
        }
        int d=garden_dither(x,y);
        row[x]=garden_rgb(10+ambient/2+sun*5/2+d,22+ambient*3/4+sun*3/2+d,28+ambient+sun/3+d);
    }
}
// ---------------------------------------------------------------------------
// The same three passes on the PIE unit. Everything above is the statement of
// what these compute; tools/pie/test_kernels.py runs the assembly below through
// an instruction-level model and compares all 240 pixels of a row against it,
// and tools/pie/run_models.py proves the arithmetic those two share.
//
// SAR is 0 for all three. Every right shift therefore costs a QACC round trip
// (EE.ZERO.QACC / EE.VMULAS.U16.QACC / EE.SRCMB.S16.QACC), which looks
// expensive until you count the alternative: SAR=11 gives shifts for one
// multiply each but takes exact low-16 multiplies away, and this kernel needs
// six of those (u*u, d0*d0, d1*d1, q*k, and t*t in each octave) which would
// then cost a QACC round trip apiece. Recounted at the instruction level the
// two settings come out within two instructions of each other per block, so
// SAR=0 stands -- not because it is better, but because the difference is
// below the 15% that instruction-cache alignment moves between builds.
//
// WARNING, and it is the reason this paragraph is here rather than in a commit
// message: tools/pie/stalls.py models neither QACC nor SAR dependencies (see
// tools/pie/README.md, its "assumptions and limits" section). The pixel pass
// takes the single accumulator twenty-two times a block, and every one of them
// is followed by an EE.SRCMB.S16.QACC that reads it back. If stalls.py reports
// zero stalls for this file, that number is silent about all of them. Read it
// as "no QR hazard", never as "runs at the issue-rate floor"; the accumulator
// question can only be settled on the device.
#ifdef ESP_PLATFORM
#define GARDEN_PIE 1
#else
#define GARDEN_PIE 0
#endif

#if GARDEN_PIE
// Broadcast each constant to sixteen bytes, once per row (or per run), so the
// loops below can walk them with plain 128-bit loads. Its own function and not
// a prologue inside each kernel, because tools/pie/stalls.py and
// tools/pie/piesim.py both read the *first* __asm__ block after a function
// name: a prologue there would be the only thing they ever analysed.
static void __attribute__((noinline))
garden_broadcast(const int16_t *k,int16_t *kv,int nk) {
    const int16_t *ks;
    int16_t *kp;
    __asm__ volatile(
        "mov            %[ks], %[k]\n"
        "mov            %[kp], %[kv]\n"
        "loopgtz        %[nk], 0f\n"
        "  ee.vldbc.16.ip  q0, %[ks], 2\n"
        "  ee.vst.128.ip   q0, %[kp], 16\n"
        "0:\n"
        : [ks]"=&a"(ks), [kp]"=&a"(kp)
        : [k]"a"(k), [kv]"a"(kv), [nk]"a"(nk)
        : "memory");
}
// The 64-pixel octave for one run of blocks: `n` blocks that begin in the same
// lattice cell, whose three corner terms arrive already differenced. Writes
// 3*D, which the 32-pixel pass below folds into.
//
// The register plan is the whole difficulty. q0 carries the lattice coordinate
// across blocks and q1/q4 carry the two constants the next block wants first;
// nothing else survives a block, because a, da and ddb ride the constant walk
// like every other constant instead of sitting in registers. That is what
// brings the live set inside eight, and it costs nothing: the sweep at
// docs/pie-simd.md:70-76 measured a fused load at zero cycles over 0..32 of
// them per block.
static void __attribute__((noinline))
garden_coarse_pie(int16_t *dens,int n,int rc0,int a,int da,int ddb) {
    int16_t k[8] __attribute__((aligned(4))) = {
        255,                      /* the cell mask, and the compare that finds the edge */
        768,                      /* smoothstep's 768-2t, taken as two subtractions */
        (int16_t)a,               /* this run's corner */
        (int16_t)da,              /* and the two differences that reach the next two */
        (int16_t)ddb,
        256,
        3,                        /* the 64-pixel octave carries three quarters of the weight */
        32                        /* 4 lattice units a pixel, eight pixels a block */
    };
    int16_t kv[8][8] __attribute__((aligned(16)));
    int16_t rcv[8] __attribute__((aligned(16)));
    for(int i=0;i<8;i++)rcv[i]=(int16_t)(rc0+4*i);
    const int16_t *kp;
    const int16_t *rp=rcv;
    int nk=8,sh8=8,sh16=16,zero=0;
    garden_broadcast(k,&kv[0][0],nk);
    __asm__ volatile(
        "wsr.sar        %[zero]\n"
        "mov            %[kp], %[kv]\n"
        "ee.vld.128.ip  q0, %[rp], 16\n"              /* the lattice coordinate, per lane */
        "ee.vld.128.ip  q4, %[kp], 16\n"              /* 255 */
        "ee.vld.128.ip  q1, %[kp], 16\n"              /* 768 */
        "loopgtz        %[n], 1f\n"
        "  ee.andq                q2, q0, q4\n"              /* t = rc & 255 */
        "  ee.vcmp.lt.s16         q3, q4, q0\n"              /* the lanes past the cell edge */
        "  ee.vsubs.s16.ld.incp   q1, %[kp], q4, q1, q2\n"   /* 768 - t;  load a */
        "  ee.vsubs.s16.ld.incp   q5, %[kp], q4, q4, q2\n"   /* 768 - 2t; load da */
        "  ee.vmul.s16.ld.incp    q6, %[kp], q2, q2, q2\n"   /* t*t, 65025 at its peak; load ddb */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc     q2, q4\n"                  /* t*t*(768-2t), too wide for a lane */
        "  ee.srcmb.s16.qacc      q2, %[sh16], 0\n"          /* f = smoothstep */
        "  ee.andq                q4, q5, q3\n"
        "  ee.vadds.s16.ld.incp   q7, %[kp], q4, q4, q1\n"   /* L = a + (da & edge); load 256 */
        "  ee.andq                q3, q6, q3\n"
        "  ee.vadds.s16           q3, q3, q5\n"              /* Q = da + (ddb & edge) */
        "  ee.vadds.s16.ld.incp   q1, %[kp], q3, q3, q4\n"   /* R = L + Q; load 3 */
        "  ee.vsubs.s16.ld.incp   q7, %[kp], q6, q7, q2\n"   /* 256 - f;  load 32 */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc     q4, q6\n"                  /* L*(256-f) */
        "  ee.vmulas.u16.qacc     q3, q2\n"                  /* + R*f, both non-negative */
        "  ee.srcmb.s16.qacc      q5, %[sh8], 0\n"           /* D */
        "  mov                    %[kp], %[kv]\n"            /* rewind, and fill the gap after SRCMB */
        "  ee.vmul.s16.ld.incp    q4, %[kp], q5, q5, q1\n"   /* 3D; load 255 for the next block */
        "  ee.vadds.s16.ld.incp   q1, %[kp], q0, q0, q7\n"   /* rc += 32; load 768 */
        "  ee.vst.128.ip          q5, %[dens], 16\n"
        "1:\n"
        : [kp]"=&a"(kp), [rp]"+a"(rp), [dens]"+a"(dens)
        : [kv]"a"(kv), [n]"a"(n), [sh8]"a"(sh8), [sh16]"a"(sh16), [zero]"a"(zero)
        : "memory");
}
// The 32-pixel octave. Identical but for the step, and for the tail: it reads
// the coarse pass's 3D back and stores the sum. The sum is left undivided --
// dens holds 3Dc+Df, not (3Dc+Df)/4 -- because the pixel pass wants
// density/12 and density/20 anyway, and floor(S/48) is floor((S/4)/12)
// exactly. That takes a whole QACC round trip out of this loop and puts
// nothing back; the only place the quarter still has to appear is the haze,
// which the pixel pass carries as 4*haze and shifts by 18 instead of 16.
static void __attribute__((noinline))
garden_fine_pie(int16_t *dens,int n,int rc0,int a,int da,int ddb) {
    int16_t k[7] __attribute__((aligned(4))) = {
        255, 768, (int16_t)a, (int16_t)da, (int16_t)ddb, 256,
        64                        /* 8 lattice units a pixel */
    };
    int16_t kv[7][8] __attribute__((aligned(16)));
    int16_t rcv[8] __attribute__((aligned(16)));
    for(int i=0;i<8;i++)rcv[i]=(int16_t)(rc0+8*i);
    const int16_t *kp;
    const int16_t *rp=rcv,*dp=dens;
    int nk=7,sh8=8,sh16=16,zero=0;
    garden_broadcast(k,&kv[0][0],nk);
    __asm__ volatile(
        "wsr.sar        %[zero]\n"
        "mov            %[kp], %[kv]\n"
        "ee.vld.128.ip  q0, %[rp], 16\n"
        "ee.vld.128.ip  q4, %[kp], 16\n"
        "ee.vld.128.ip  q1, %[kp], 16\n"
        "loopgtz        %[n], 1f\n"
        "  ee.andq                q2, q0, q4\n"
        "  ee.vcmp.lt.s16         q3, q4, q0\n"
        "  ee.vsubs.s16.ld.incp   q1, %[kp], q4, q1, q2\n"
        "  ee.vsubs.s16.ld.incp   q5, %[kp], q4, q4, q2\n"
        "  ee.vmul.s16.ld.incp    q6, %[kp], q2, q2, q2\n"
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc     q2, q4\n"
        "  ee.srcmb.s16.qacc      q2, %[sh16], 0\n"
        "  ee.andq                q4, q5, q3\n"
        "  ee.vadds.s16.ld.incp   q7, %[kp], q4, q4, q1\n"
        "  ee.andq                q3, q6, q3\n"
        "  ee.vadds.s16           q3, q3, q5\n"
        "  ee.vadds.s16.ld.incp   q1, %[kp], q3, q3, q4\n"   /* R; load 64 */
        "  ee.vsubs.s16.ld.incp   q7, %[dp], q6, q7, q2\n"   /* 256-f; load the coarse pass's 3D */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc     q4, q6\n"
        "  ee.vmulas.u16.qacc     q3, q2\n"
        "  ee.srcmb.s16.qacc      q5, %[sh8], 0\n"
        "  mov                    %[kp], %[kv]\n"
        "  ee.vadds.s16.ld.incp   q4, %[kp], q5, q5, q7\n"   /* S = 3Dc + Df; load 255 */
        "  ee.vadds.s16.ld.incp   q1, %[kp], q0, q0, q1\n"   /* rc += 64; load 768 */
        "  ee.vst.128.ip          q5, %[dens], 16\n"
        "1:\n"
        : [kp]"=&a"(kp), [rp]"+a"(rp), [dp]"+a"(dp), [dens]"+a"(dens)
        : [kv]"a"(kv), [n]"a"(n), [sh8]"a"(sh8), [sh16]"a"(sh16), [zero]"a"(zero)
        : "memory");
}
// The pixel pass. Forty constants, each loaded where it is used and nowhere
// held: the walk is monotone, so the table below is in issue order and reading
// the two side by side is the only way to check either. Repeats are repeats on
// purpose -- 256 appears four times because it is wanted four times, and a
// register kept for it would have to come out of the seven the arithmetic uses.
//
// Twenty-one of the forty now ride a .LD.INCP form (see the macros below and
// GARDEN_PIE_FUSE); the other nineteen are plain, because only
// EE.VADDS/VSUBS/VMUL have that form, the host has to be free at the right
// point in the walk, and a fusion that would put a stage-2 load next to its
// consumer is refused. 135 instructions a block, 114 fused. The estimate from
// counting the arithmetic was ~107; 114 is what the register allocation, the
// walk order and the stall check actually allow.
//
// tools/pie/fuse_pixels.py generated this and can regenerate it. Edit the
// arithmetic here; do not hand-edit which load rides which instruction.
//
// The body is ~470 bytes, well past loopgtz's 256, so it closes with addi/bnez
// (docs/pie-simd.md 7). Two instructions a block.
// The fusion, as one spelling with a switch under it.
//
// EE.VADDS/VSUBS/VMUL have a .LD.INCP form that loads a vector and advances the
// pointer in the same slot, and 3.4 measures that load as free -- so a fused
// load is an instruction that has stopped existing. Twenty-one of the pixel
// pass's forty constants can ride one: 135 instructions a block become 114.
//
// The unfused expansion is the same instruction in the same place followed by
// the plain load, so the two builds issue identical work in identical order and
// differ only by the twenty-two slots. That is what makes the switch a control
// rather than a second kernel: comparing across two builds is exactly how a
// 1.0 ms figure got produced that turned out to be inside the i-cache
// alignment envelope.
//
// The one invariant that is not local: the constant walk is the PROGRAM ORDER
// of the loads. A fused load moves to its host instruction, and if that move
// crossed another load the two would silently swap constants -- a defect that
// compiles, runs, and shows up as a wrong colour somewhere. The generator that
// produced this body checked the order of all forty against the original;
// tools/pie/test_kernels.py checks the result against the scalar loop, which is
// what would actually catch it.
#if GARDEN_PIE_FUSE
#define P_ADD(z,x,y,ld) "  ee.vadds.s16.ld.incp " ld ", %[kp], " z ", " x ", " y "\n"
#define P_SUB(z,x,y,ld) "  ee.vsubs.s16.ld.incp " ld ", %[kp], " z ", " x ", " y "\n"
#define P_MUL(z,x,y,ld) "  ee.vmul.s16.ld.incp " ld ", %[kp], " z ", " x ", " y "\n"
#else
#define P_ADD(z,x,y,ld) "  ee.vadds.s16 " z ", " x ", " y "\n" \
                        "  ee.vld.128.ip " ld ", %[kp], 16\n"
#define P_SUB(z,x,y,ld) "  ee.vsubs.s16 " z ", " x ", " y "\n" \
                        "  ee.vld.128.ip " ld ", %[kp], 16\n"
#define P_MUL(z,x,y,ld) "  ee.vmul.s16 " z ", " x ", " y "\n" \
                        "  ee.vld.128.ip " ld ", %[kp], 16\n"
#endif
static void __attribute__((noinline))
garden_pixels_pie(uint16_t *row,const int16_t *dens,const GardenRow *r) {
    int center=r->center,width=r->width,mww=r->mww;
    int at0=r->at0,w0=r->w0,mw0=r->mw0;
    int at1=r->at1,w1=r->w1,mw1=r->mw1;
    int ay=r->ambient_y,kb=r->kbase,dy=r->dy;
    int16_t k[40] __attribute__((aligned(4))) = {
        (int16_t)(-center), (int16_t)width, (int16_t)(-width),
        (int16_t)mww,                        /* ceil(2^22/ww), with a shift of 14 */
        256,
        (int16_t)((65536+47)/48),            /* density/12, taken on the undivided sum */
        (int16_t)ay,
        (int16_t)((65536+79)/80),            /* density/20, likewise */
        (int16_t)kb,
        320,                                 /* 4*haze, so the shoulder shifts by 18 */
        (int16_t)(-at0), (int16_t)w0, (int16_t)(-w0), (int16_t)mw0, 256, 4,
        (int16_t)(-at1), (int16_t)w1, (int16_t)(-w1), (int16_t)mw1, 256, 7,
        GARDEN_DKX, (int16_t)dy, GARDEN_DKM, 3,
        128, 640, 10, 256, (int16_t)0xF800,  /* red: (ambient + 5 sun) >> 1, then placed */
        192, 384, 22, 8, 0x07E0,             /* green: (3 ambient + 6 sun) >> 2 */
        GARDEN_M3, 28, 32,                   /* blue: no left shift to hide the >>3 in */
        8                                    /* eight pixels on */
    };
    int16_t kv[40][8] __attribute__((aligned(16)));
    int16_t xv[8] __attribute__((aligned(16)))={0,1,2,3,4,5,6,7};
    const int16_t *kp;
    const int16_t *xp=xv;
    int nk=40,cnt=30,sh8=8,sh14=14,sh16=16,sh17=17,sh18=18,zero=0;
    garden_broadcast(k,&kv[0][0],nk);
    __asm__ volatile(
        "wsr.sar        %[zero]\n"
        "mov            %[kp], %[kv]\n"
        "ee.vld.128.ip  q0, %[xp], 16\n"                     /* x, per lane */
        "1:\n"
        "  ee.vld.128.ip        q1, %[kp], 16\n"             /* -center */
        "  ee.vld.128.ip        q2, %[kp], 16\n"             /* width */
        P_ADD("q3","q0","q1","q1")                  /* u = x - center; load -width */
        "  ee.vmin.s16          q3, q3, q2\n"
        "  ee.vmax.s16          q3, q3, q1\n"                /* clamped, so the lobe needs no test */
        P_MUL("q3","q3","q3","q2")                  /* u*u, at most 10404; load ceil(2^22/ww) */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q3, q2\n"
        "  ee.vld.128.ip        q2, %[kp], 16\n"             /* 256 */
        "  ee.srcmb.s16.qacc    q3, %[sh14], 0\n"
        P_SUB("q3","q2","q3","q4")                  /* q, exactly 0 at the clamp; load ceil(2^16/48) */
        "  ee.vld.128.ip        q5, %[dens], 16\n"           /* S = 3Dc + Df */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q5, q4\n"
        "  ee.vld.128.ip        q4, %[kp], 16\n"             /* ambient_y */
        "  ee.srcmb.s16.qacc    q1, %[sh16], 0\n"
        P_ADD("q1","q1","q4","q2")                  /* ambient; load ceil(2^16/80) */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q5, q2\n"
        "  ee.vld.128.ip        q4, %[kp], 16\n"             /* 17 + breath */
        "  ee.srcmb.s16.qacc    q2, %[sh16], 0\n"
        "  ee.vld.128.ip        q6, %[kp], 16\n"             /* 320 */
        "  ee.vadds.s16         q2, q2, q4\n"                /* k */
        "  ee.vadds.s16         q5, q5, q6\n"                /* 4*haze */
        P_MUL("q4","q3","q2","q2")                  /* q*k, at most 8704; load -at0 */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q4, q3\n"
        "  ee.srcmb.s16.qacc    q4, %[sh16], 0\n"            /* sun */
        P_ADD("q3","q0","q2","q6")                  /* load w0 */
        "  ee.vld.128.ip        q2, %[kp], 16\n"             /* -w0 */
        "  ee.vmin.s16          q3, q3, q6\n"
        "  ee.vmax.s16          q3, q3, q2\n"
        P_MUL("q3","q3","q3","q6")                  /* load ceil(2^22/ww0) */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q3, q6\n"
        "  ee.vld.128.ip        q6, %[kp], 16\n"             /* 256 */
        "  ee.srcmb.s16.qacc    q3, %[sh14], 0\n"
        P_SUB("q3","q6","q3","q7")                  /* the shoulder, 0..256; load 4 */
        "  ee.vld.128.ip        q2, %[kp], 16\n"            /* -at1 */
        P_MUL("q6","q3","q7","q7")                  /* s*4 keeps s*s inside a lane; load w1 */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q6, q3\n"
        "  ee.srcmb.s16.qacc    q6, %[sh8], 0\n"
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q6, q5\n"
        "  ee.srcmb.s16.qacc    q6, %[sh18], 0\n"
        P_ADD("q4","q4","q6","q3")                  /* sun += the first shoulder; load -w1 */
        "  ee.vadds.s16         q6, q0, q2\n"
        "  ee.vmin.s16          q6, q6, q7\n"
        "  ee.vmax.s16          q6, q6, q3\n"
        P_MUL("q6","q6","q6","q7")                  /* load ceil(2^22/ww1) */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q6, q7\n"
        "  ee.vld.128.ip        q7, %[kp], 16\n"             /* 256 */
        "  ee.srcmb.s16.qacc    q6, %[sh14], 0\n"
        P_SUB("q6","q7","q6","q3")                  /* load the dither's x multiplier; load 7 */
        "  ee.vld.128.ip        q2, %[kp], 16\n"
        P_MUL("q7","q6","q3","q3")                  /* load its row term */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q7, q6\n"
        "  ee.srcmb.s16.qacc    q7, %[sh8], 0\n"
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q7, q5\n"
        "  ee.srcmb.s16.qacc    q7, %[sh18], 0\n"
        P_MUL("q5","q0","q2","q6")                  /* 4*haze is finished with; load the dither's second multiplier */
        "  ee.vadds.s16         q4, q4, q7\n"                /* sun += the second shoulder */
        "  ee.xorq              q5, q5, q3\n"                /* XOR, because EE.VADDS saturates */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q5, q5\n"                    /* the full 32-bit square */
        "  ee.srcmb.s16.qacc    q5, %[sh17], 0\n"   /* 17: SRCMB saturates, h*h reaches 2^32 */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q5, q6\n"
        "  ee.vld.128.ip        q6, %[kp], 16\n"             /* 3 */
        "  ee.srcmb.s16.qacc    q5, %[sh16], 0\n"
        "  ee.vld.128.ip        q7, %[kp], 16\n"             /* 128 */
        "  ee.andq              q3, q5, q6\n"                /* the dither, 0..3 */
        "  ee.vld.128.ip        q6, %[kp], 16\n"             /* 640 */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q1, q7\n"                    /* 128*ambient */
        "  ee.vmulas.u16.qacc   q4, q6\n"                    /* + 640*sun, so >>8 is (a+5s)>>1 */
        "  ee.srcmb.s16.qacc    q2, %[sh8], 0\n"
        P_ADD("q2","q2","q3","q7")                  /* load 256; load 10 */
        "  ee.vld.128.ip        q6, %[kp], 16\n"
        P_ADD("q2","q2","q7","q7")                  /* load 0xF800 */
        "  ee.vmul.s16          q2, q2, q6\n"                /* (v>>3)<<11 is (v<<8) masked */
        "  ee.vld.128.ip        q5, %[kp], 16\n"             /* 192 */
        "  ee.andq              q2, q2, q7\n"
        "  ee.vld.128.ip        q6, %[kp], 16\n"             /* 384 */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q1, q5\n"
        "  ee.vmulas.u16.qacc   q4, q6\n"
        "  ee.srcmb.s16.qacc    q5, %[sh8], 0\n"
        P_ADD("q5","q5","q3","q7")                  /* load 8; load 22 */
        "  ee.vld.128.ip        q6, %[kp], 16\n"
        P_ADD("q5","q5","q7","q7")                  /* load 0x07E0 */
        "  ee.vmul.s16          q5, q5, q6\n"
        "  ee.vld.128.ip        q6, %[kp], 16\n"             /* ceil(2^16/3) */
        "  ee.andq              q5, q5, q7\n"
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q4, q6\n"                    /* sun/3 */
        "  ee.srcmb.s16.qacc    q7, %[sh16], 0\n"
        P_ADD("q7","q7","q1","q6")                  /* load 28 */
        P_ADD("q7","q7","q3","q1")                  /* load 32 */
        P_ADD("q7","q7","q6","q6")                  /* load 8 */
        "  ee.zero.qacc\n"
        "  ee.vmulas.u16.qacc   q7, q1\n"                    /* 32*v >> 8 is v >> 3 */
        "  ee.srcmb.s16.qacc    q7, %[sh8], 0\n"
        "  ee.orq               q2, q2, q5\n"
        "  ee.orq               q2, q2, q7\n"
        "  ee.vst.128.ip        q2, %[row], 16\n"
        "  ee.vadds.s16         q0, q0, q6\n"                /* eight pixels on */
        "  mov                  %[kp], %[kv]\n"
        "  addi                 %[cnt], %[cnt], -1\n"
        "  bnez                 %[cnt], 1b\n"
        : [kp]"=&a"(kp), [xp]"+a"(xp), [dens]"+a"(dens), [row]"+a"(row), [cnt]"+a"(cnt)
        : [kv]"a"(kv), [sh8]"a"(sh8), [sh14]"a"(sh14), [sh16]"a"(sh16),
          [sh17]"a"(sh17), [sh18]"a"(sh18), [zero]"a"(zero)
        : "memory");
}
#undef P_ADD
#undef P_SUB
#undef P_MUL
#endif

// The run structure both spellings share: blocks that begin in the same
// lattice cell, handed over with their three corner terms already differenced.
// A run never splits a block -- the one that straddles a cell edge stays in the
// run it began in, and its far lanes are switched over by the compare-and-mask
// inside the loop -- which is what keeps every store 16-byte aligned. Cutting
// at the true edge instead would start most runs at an odd pixel, and a
// misaligned 128-bit store does not fault, it silently rounds the address down.
static void garden_octave_row(int16_t *dens,const int *v,int mask,int p,int step,
                              int first) {
    for(int b=0;b<30;) {
        int g=b*8*step+p,cc=g>>8,rc0=g&255;
        int n=(255-rc0)/(step*8)+1;
        if(b+n>30)n=30-b;
        int a=v[cc&mask],da=v[(cc+1)&mask]-a;
        int ddb=v[(cc+2)&mask]-v[(cc+1)&mask]-da;
#if GARDEN_PIE
        if(first)garden_coarse_pie(dens+b*8,n,rc0,a,da,ddb);
        else garden_fine_pie(dens+b*8,n,rc0,a,da,ddb);
#else
        garden_octave_lanes(dens+b*8,n,rc0,step,a,da,ddb,first);
#endif
        b+=n;
    }
}
// The row's whole share of the lane pipeline: fill in the constants, lay the
// two noise octaves into `dens`, then turn them into pixels. `dens` is 16-byte
// aligned because the vector store rounds a misaligned address down instead of
// faulting (docs/pie-simd.md 5), and it is on the stack rather than in the
// scene block because it lives for one row and the ui task has 32 KB to spare.
//
// Split out of garden_row so that tools/test_garden.c can hold it against
// garden_row_scalar pixel for pixel, which it cannot do through the trunks and
// the canopy drawn on top.
// One mote, on one row, gated and drawn.
//
// Split out so that the two ways of reaching it -- the index, and the scan it
// replaced -- draw identically by construction. Two copies of this that had to
// stay in step would be exactly the kind of switch that quietly stops
// describing the thing it is meant to subtract.
static inline void garden_mote_touch(uint16_t *row,int y,const GardenMote *m,
                                     const int16_t *dens,const GardenRow *r) {
    if(!m->glow)return;
    // Split between the two rows it lies across, by the sixteenths of its
    // position. That is what turns a quarter-pixel-a-frame climb into a
    // drift instead of a stutter: the mote dims on one row as it brightens
    // on the next, and never sits still.
    int top=m->y>>4,frac=m->y&15,share;
    if(top==y)share=16-frac;
    else if(top+1==y)share=frac;
    else return;
    if(!share)return;
    int glow=m->glow*share>>4;
    // The ramp still earns its place: deaths are visible now, but a birth
    // is still a particle appearing where there was none, and a replacement
    // that blinks on is the one thing left that would read as a glitch.
    if(m->age<8)glow=glow*(m->age+1)>>3;
    // And the dying fade, through the same term that gives it the shaft's
    // colour -- so it dims into the light rather than needing a second
    // blend path.
    if(m->dir<0)glow=glow*m->dim/GARDEN_DYING;
    if(!glow)return;
    int mx=m->x>>4;
    // The point and its two neighbours at half. Three across is what a mote
    // is -- one pixel alone flickers as it crosses a column boundary, and
    // anything wider stops reading as a point of light.
    for(int k=-1;k<=1;k++) {
        int x=mx+k;
        if(x<0||x>=240)continue;
#if GARDEN_MOTE_MAGENTA
        // Straight to the panel, past everything. See the switch in garden.h
        // for what this can and cannot establish: it is the coordinates and the
        // write, not the arithmetic.
        (void)dens;(void)r;(void)glow;
        row[x]=0xF81Fu;
#else
        row[x]=garden_shade_pixel(x,dens[x],r,k?glow/2:glow);
#endif
    }
}

static void garden_pixels_row(uint16_t *row,int y,const GardenFrame *f) {
    int center,width;garden_shaft(y,f,&center,&width);
    // Everything below is invariant across the row. It used to be recomputed
    // 240 times per row because it sat inside the pixel loop: the trunk hashes
    // and their three integer divisions alone were nine divide instructions per
    // pixel. The divisor of every remaining per-pixel division is hoisted too,
    // and then replaced by a reciprocal, so the loop does not divide at all.
    int ambient_y=20+y/15,ww=width*width;
    // Two faint, unequal openings inside the same sunlight volume. Their
    // shoulders dissolve into the haze; no repeated luminous stripes.
    int at0=center-18,w0=10+y/13,ww0=w0*w0;
    int at1=center+27,w1=10+y/8,ww1=w1*w1;
    GardenRow r;
    r.center=center;r.width=width;r.mww=garden_recip(ww,22);
    r.at0=at0;r.w0=w0;r.mw0=garden_recip(ww0,22);
    r.at1=at1;r.w1=w1;r.mw1=garden_recip(ww1,22);
    r.ambient_y=ambient_y;r.kbase=17+f->breath;
    r.dy=(y*GARDEN_DKY+GARDEN_DKC)&0xffff;
    // px>>5 and px>>6 are 8x+(phase>>5) and 4x+(phase>>6) exactly: x*256 is a
    // multiple of 64, so the shift never mixes x with the phase's low bits.
    r.p5=f->phase>>5;r.p6=f->phase>>6;
    for(int i=0;i<4;i++)r.vc[i]=garden_corner(i,3,y,6);
    for(int i=0;i<8;i++)r.vf[i]=garden_corner(i,7,y,5);
    int16_t dens[240] __attribute__((aligned(16)));
    garden_octave_row(dens,r.vc,3,r.p6,4,1);
    garden_octave_row(dens,r.vf,7,r.p5,8,0);
#if GARDEN_PIE
    garden_pixels_pie(row,dens,&r);
#else
    garden_pixels(row,dens,&r);
#endif
#if GARDEN_MOTE_ONLY
    // The blanking, and it is deliberately here rather than in place of the
    // kernel call above: the kernel still runs, `dens` and `r` are still what
    // they always were, and the mote touch-up below reads exactly what it reads
    // in a shipping build. Only the background's pixels are thrown away.
    for(int x=0;x<240;x++)row[x]=0;
#endif
    // Dust in the shaft, after the light and before the trunks -- so the motes
    // are in the beam and the trunks and canopy occlude them, which is the
    // order they should be in. This is where it belongs because `dens` and `r`
    // are still alive: a mote's pixel is rebuilt from them in about thirty
    // operations, and storing all 240 lanes of `sun` to avoid that would cost
    // one extra vector store per block, about 27 us a frame, against roughly
    // five for the few dozen pixels the motes actually touch.
    //
    // That last number is the drawing, and the drawing was never the cost.
    // The first version tested all fourteen particles on all 135 rows and cost
    // 1.0 ms of a 41 ms frame -- measured on the board, against 4.6 us
    // predicted from the pixels touched, which is wrong by a factor of two
    // hundred. 1,890 tests to find 28 hits: the decision, not the draw. The
    // index in garden.h is that decision moved into the frame; what is left
    // here is a bitmask load and the hits.
#if GARDEN_NO_MOTES
    /* nothing: garden_mote_touch is a static inline and simply goes away */
#elif GARDEN_MOTE_INDEX
    for(unsigned mask=f->rowmask[y];mask;mask&=mask-1)
        garden_mote_touch(row,y,&f->mote[__builtin_ctz(mask)],dens,&r);
#else
    for(int i=0;i<GARDEN_MOTES;i++)
        garden_mote_touch(row,y,&f->mote[i],dens,&r);
#endif
    // Adding something to the light afterwards, per pixel: this is where it
    // goes, and this is why `dens` and `r` are still in scope at the end of a
    // function that has finished drawing.
    //
    // The packing is the last thing that happens to `sun` -- it is in a lane
    // register, it is turned into RGB565, and the block moves on -- so nothing
    // downstream can add to it. But it is not lost either. `sun` is a function
    // of dens[x] and this row's constants and nothing else, so one pixel's
    // worth can be rebuilt here in about thirty scalar operations without
    // touching the packed colour: the expensive input, the two octaves of
    // noise, is the array sitting right there. Rebuilding a handful of pixels
    // costs less than storing all 240 lanes of `sun` would have -- one extra
    // EE.VST.128.IP a block is 1.6 cycles x 30 x 135, about 27 us a frame,
    // against roughly 5 us to rebuild the few dozen pixels anything sparse
    // would actually touch. So the kernel does not store it, on purpose.
    //
    // Both halves of that comparison are pixel counts, and pixel counts turned
    // out to be the wrong unit for the sparse side: the first swarm cost 1.0 ms
    // rather than the 5 us this arithmetic gives, because none of it was in the
    // rebuilding. It was in finding out which pixels to rebuild. The 27 us
    // stands -- it is a fixed store on every block and nothing decides anything
    // -- and the choice not to store `sun` still holds, but only once the
    // deciding is somewhere other than the row.
    //
    // Two scales to get right, because both are four times what the scalar
    // reference above calls by the same name, and a wrong power of two here
    // looks plausible rather than broken -- the same shape of defect as the
    // reciprocal that dropped a factor of 256 and inverted the lobe at its own
    // edge. `dens` holds 3Dc+Df, four times `density`. `haze` is carried as
    // 320+dens, four times `80+density`. `sun` itself is unscaled: it is the
    // same 0..48 the scalar loop produces, so a term added to it is in the
    // units where the lobe peaks near 34, and it reaches the three channels
    // through the same >>1, >>2 and /3 as everything else.
}
void garden_row(uint16_t *row,int y,const GardenFrame *f) {
    // Distant trunks disappear into the air rather than reading as sharp
    // cutouts. Near vegetation below is darker and has greater contrast.
    int trunk[3];
    for(int i=0;i<3;i++) {
        unsigned h=garden_hash((unsigned)i+901+f->seed);
        trunk[i]=16+i*86+(int)(h%37)+(y-70)*((int)((h>>8)%5)-2)/19;
    }
    const uint16_t bark=garden_rgb(18,36,39),leafy=garden_rgb(9,29,25);
#ifdef ESP_PLATFORM
    GARDEN_FENCE;uint32_t pt0=esp_cpu_get_cycle_count();GARDEN_FENCE;
    garden_pixels_row(row,y,f);
    GARDEN_FENCE;garden_pixel_cycles+=esp_cpu_get_cycle_count()-pt0;GARDEN_FENCE;
#else
    garden_pixels_row(row,y,f);
#endif
    // The trunks blend each pixel independently and in the same i order, so
    // running them as three short passes after the row is written produces the
    // identical image while touching the fifteen columns where 70-9*dist>0
    // instead of testing all 240 three times.
    for(int i=0;i<3;i++) {
        int lo=trunk[i]-7,hi=trunk[i]+7;
        if(lo<0)lo=0;
        if(hi>239)hi=239;
        for(int x=lo;x<=hi;x++) {
            int opacity=70-abs(x-trunk[i])*9;
            if(opacity>0)row[x]=garden_mix(row[x],bark,(unsigned)opacity);
        }
    }
    // Out-of-focus canopy at the top: broad soft ellipses break up the light.
    for(int i=0;i<7;i++) {
        unsigned h=garden_hash((unsigned)i+301+f->seed);
        int cy=-9+(int)(h%17),dy=y-cy,ry=15+(int)((h>>8)%16);
        if(abs(dy)>=ry)continue;
        int cx=i*40-15+(int)((h>>20)%23)
            +(garden_motion((unsigned)(f->phase+i*180),613)-128)/48;
        int rx=28+(int)((h>>16)%17);
        // Secondary PIE candidate: contiguous ellipse coverage + RGB565 blend.
        // Clip once, process aligned 8-pixel interiors, retain scalar tails;
        // do not assume the caller's row pointer is 16-byte aligned.
        // The vertical term does not depend on x; clipping once also drops the
        // per-pixel bounds test.
        // The one per-pixel division left in this file, and the only one that
        // was never hoisted: rr is invariant for the whole ellipse-row and the
        // divide ran on all ~10,450 pixels the canopy touches in a frame.
        //
        // Exactly, not approximately. The lobe's reciprocals rounded up at 2^22
        // and moved the picture by a step; here the domain is small enough
        // (rr = rx*rx for rx in 28..44, and |dx| <= rx so the numerator never
        // exceeds rr) that ceil(2^26/rr) with a shift of 18 reproduces
        // dx*dx*256/rr for every reachable pair. garden_model.c sweeps it.
        int qy=dy*dy*256/(ry*ry),rr=rx*rx;
        int mrr=garden_recip(rr,26);
        int lo=cx-rx,hi=cx+rx;
        if(lo<0)lo=0;
        if(hi>239)hi=239;
        garden_canopy_row(row,lo,hi,cx,mrr,qy,leafy);
    }
    // Curved grass and paired fern leaflets. Hashes describe plants, not stored
    // geometry. Each row intersects only a few spans, never a screen buffer.
    for(int i=0;i<18;i++) {
        unsigned h=garden_hash((unsigned)i+71+f->seed);
        int root=(i/2)*30-15+(int)((h>>16)%27);
        // Static density makes clumps and gaps; only the wind evolves in time.
        if((int)((h>>24)&255)>70+garden_noise((unsigned)(root+32)*5,727+f->seed)*3/4)continue;
        int layer=i&1,height=23+(int)(h%63),base=142+layer*7;
        int up=base-y;if(up<0||up>height)continue;
        int t=up*256/height,lean=(int)((h>>8)%31)-15;
        int wind=(garden_motion((unsigned)(f->phase+(root+32)*48),557)-128)/12;
        int cx=root+(lean+wind*(layer+1)/2)*t*t/65536;
        int radius=1+(256-t)*(1+(int)((h>>14)&1))/220;
        uint16_t green=garden_rgb(layer?16:26,layer?59:67,layer?39:53);
        for(int x=cx-radius;x<=cx+radius;x++)if(x>=0&&x<240)
            row[x]=garden_mix(row[x],green,layer?220:130);
        if((h&3)==0&&t>40&&t<220) {
            int spacing=8+(int)((h>>12)%5);
            int band=up%spacing,leaf=spacing-abs(band-spacing/2)*2;
            int reach=leaf*(256-t)/220;
            for(int dx=-reach;dx<=reach;dx++) {
                int x=cx+dx;if(x<0||x>=240)continue;
                row[x]=garden_mix(row[x],green,layer?180:110);
            }
        }
    }
}