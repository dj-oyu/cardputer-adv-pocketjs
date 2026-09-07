#include "garden.h"
#include <math.h>
#include <stdlib.h>

// No writable statics, LUTs, images or vertex lists. A broad warm scattering
// lobe sits in cool mist, broken by drifting density and canopy silhouettes.
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
static int garden_density(GardenSpan *s,int px,int y,int shift) {
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
static uint16_t garden_rgb(int r,int g,int b) {
    return (uint16_t)((r>>3)<<11|(g>>2)<<5|(b>>3));
}
static uint16_t garden_mix(uint16_t a,uint16_t b,unsigned f) {
    unsigned r=(((a>>11)&31)*(256-f)+((b>>11)&31)*f)>>8;
    unsigned g=(((a>>5)&63)*(256-f)+((b>>5)&63)*f)>>8;
    unsigned blue=((a&31)*(256-f)+(b&31)*f)>>8;
    return (uint16_t)(r<<11|g<<5|blue);
}
void garden_prepare(GardenFrame *f,float time) {
    // Fractional advection avoids whole-pixel jumps. All noise is periodic at
    // this wrap, including wind, so long-running animation has no reset seam.
    f->phase=(int)(fmodf(fmaxf(time,0),128.0f)*512);
    f->sun=(garden_motion((unsigned)f->phase,83)-128)/24;
    f->breath=(garden_motion((unsigned)f->phase,193)-128)/24;
    f->seed=0;
}
void garden_row(uint16_t *row,int y,const GardenFrame *f) {
    int center=200-(y+40)*3/4+f->sun,width=58+y/3;
    GardenSpan fine={-1,0,0},coarse={-1,0,0};
    int warp=(garden_motion((unsigned)(f->phase+y*64),419)-128)/16;
    center+=warp;
    // PIE candidate (profile on device first): this full-width loop runs on
    // every row. Batch light polynomials, RGB565 packing and trunk blends in
    // 8-pixel fixed-point lanes; hoist row-invariant divisions first. Keep
    // lattice/hash work separate and split batches at moving cell boundaries.
    // Prove intermediate ranges before narrowing: squares/products exceed
    // 16 bits. See docs/pie-simd.md for SAR, multiply and alignment constraints;
    // use scalar edges and verify rounding against this scalar reference.
    for(int x=0;x<240;x++) {
        // Two octaves, 64/32 px; cache lattice corners on the stack per row.
        // The broad field dominates, retaining directional shafts, not smoke.
        int px=x*256+f->phase;
        int density=(3*garden_density(&coarse,px,y,6)
                       +garden_density(&fine,px,y,5))/4;
        int ambient=20+density/12+y/15;
        int u=x-center,q=256-u*u*256/(width*width);
        int sun=q>0?q*q*(17+f->breath+density/20)/65536:0;
        // Two faint, unequal openings inside the same sunlight volume. Their
        // shoulders dissolve into the haze; no repeated luminous stripes.
        for(int i=0;i<2;i++) {
            int at=center+(i?27:-18),w=10+y/(i?8:13),d=x-at;
            int shoulder=256-d*d*256/(w*w);
            if(shoulder>0)sun+=shoulder*shoulder*(i?7:4)*(80+density)/(65536*256);
        }
        // Stable one-bit spatial dither softens RGB565 steps without flicker.
        int d=(int)((garden_hash((unsigned)(x+y*240))>>8)&3);
        row[x]=garden_rgb(10+ambient/2+sun*5/2+d,22+ambient*3/4+sun*3/2+d,28+ambient+sun/3+d);
        // Distant trunks disappear into the air rather than reading as sharp
        // cutouts. Near vegetation below is darker and has greater contrast.
        for(int i=0;i<3;i++) {
            unsigned h=garden_hash((unsigned)i+901+f->seed);
            int trunk=16+i*86+(int)(h%37)+(y-70)*((int)((h>>8)%5)-2)/19,dist=abs(x-trunk);
            int opacity=70-dist*9;
            if(opacity>0)row[x]=garden_mix(row[x],garden_rgb(18,36,39),(unsigned)opacity);
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
        for(int x=cx-rx;x<=cx+rx;x++)if(x>=0&&x<240) {
            int dx=x-cx,q=256-dx*dx*256/(rx*rx)-dy*dy*256/(ry*ry);
            if(q>0)row[x]=garden_mix(row[x],garden_rgb(9,29,25),(unsigned)q*3/5);
        }
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
