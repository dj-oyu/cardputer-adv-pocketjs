#include "flower.h"
#include "scene_mem.h"
#include "garden.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

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
    float radius[3],q[6],invzz;
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
static float bell_slopes[LAT],bell_offsets[LAT];
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
static float elapsed,tx,ty;

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
static V add(V a,V b) { return (V){a.x+b.x,a.y+b.y,a.z+b.z}; }
static V mul(V a,float b) { return (V){a.x*b,a.y*b,a.z*b}; }
static float dot(V a,V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
static V cross(V a,V b) {return (V){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static V normal(V a) { return mul(a,1.0f/sqrtf(fmaxf(dot(a,a),1e-12f))); }
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
static void cup(V root,float size,unsigned material,float yaw,float pitch) {
    // Six tepals in two whorls: upright, overlapping ellipsoidal surfaces.
    for(int i=0;i<6;i++) {
        float a=i*PI/3+.25f,r=((material==VIOLET?.39f:.27f)+.025f*sinf(elapsed*.8f))*size;
        V bottom=add(root,(V){.045f*size*cosf(a),0,.045f*size*sinf(a)});
        V top=add(root,(V){r*cosf(a),size*(.94f+(i%2)*.045f),r*sinf(a)});
        part(bottom,top,size*.20f,size*.075f,material,yaw,pitch);
    }
}
static void botanicals(float yaw,float pitch) {
    count=0;
    float sway=.04f*sinf(elapsed*.7f),breath=.025f*sinf(elapsed*.8f);
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
            float a=i*2*PI/3+.2f+tx*.25f;
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
        V head={.04f,.02f,0};
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
    float response=1-expf(-dt*5);
    tx+=(clampi(tilt_x,-180,180)/512.0f-tx)*response;
    ty+=(clampi(tilt_y,-180,180)/512.0f-ty)*response;
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
    float yaw=elapsed*.05f+tx, pitch=.48f+ty;
    count=PETALS;
    if(current_species!=FLOWER_CRYSTAL) {
        // Plants sway around their roots rather than rotating upside down.
        yaw=tx*.18f+.035f*sinf(elapsed*.6f);pitch=.12f+ty*.3f;
        botanicals(yaw,pitch);
    }
    for(unsigned i=0;i<count;i++) {
        Petal *p=&petals[i];
        if(current_species==FLOWER_CRYSTAL) {
        float a=i*2*PI/7, breath=.5f+.5f*sinf(elapsed*.65f);
        float bend=.15f+.23f*breath+.08f*sinf(elapsed*.5f+i*.8f);
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
        for(int j=0;j<3;j++) {
            V b=p->axis[j];float r=p->radius[j],k=1/(r*r);
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
    for(int i=0;i<LAT;i++) {
        float t=(float)(i+1)/LAT,k=2*t-1;
        float radius=t<.5f?sqrtf(fmaxf(0,1-(1-2*t)*(1-2*t)))*.88f:.88f+.24f*k*k*k;
        bell_slopes[i]=(radius-previous)*LAT*.5f;
        bell_offsets[i]=previous-bell_slopes[i]*(-1+2.0f*i/LAT);
        previous=radius;
    }
    seeds_ready=true;
}
static uint16_t shade(V n,int petal,V hit) {
    unsigned material=petals[petal].material;
    bool inside=n.z<0;
    n=normal(n);
    if(inside)n=mul(n,-1);
    float diffuse=fmaxf(0,dot(n,(V){-.36f,.48f,.8f}));
    float rim=1-fmaxf(0,n.z);rim*=rim;
    float spec=fmaxf(0,dot(n,(V){-.19f,.25f,.949f}));
    spec*=spec;spec*=spec;spec*=spec;spec*=spec;
    float band=fmaxf(0,1-fabsf(n.x*.65f+n.y*.3f-.18f)*6);
    band=band*band*.22f;
    if(material>=LEAF) {
        const Petal *p=&petals[petal];V local=add(hit,mul(p->c,-1));
        float longitudinal=dot(local,p->axis[0])/p->radius[0];
        float transverse=dot(local,p->axis[1])/p->radius[1];
        float light=.30f+.66f*diffuse;
        if(inside)light*=material==GOLD?.8f:.52f;
        if(p->shape==2)light=.55f+.43f*diffuse;
        float r=0,g=0,b=0;
        if(material==LEAF) {
            float vein=fmaxf(0,1-fabsf(transverse)*12)*.16f;
            r=22;g=105+vein*160;b=53+vein*100;spec*=.35f;
        } else if(material==GOLD) {
            r=255;g=165+25*longitudinal;b=13;
            if(p->shape) {g=220;b=47;light=.70f+.28f*diffuse;spec*=.3f;}
        }
        else if(material==ROSE) {r=242;g=47+35*longitudinal;b=104+32*longitudinal;}
        else if(material==VIOLET) {r=139+34*longitudinal;g=65+20*longitudinal;b=235;}
        else if(material==SEED) {
            int x=clampi((int)(16+15*longitudinal),0,31),y=clampi((int)(16+15*transverse),0,31);
            float seed=seed_map[y*32+x]/255.0f;
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
static bool bell_hit(const Petal *p,float dx,float dy,float *best,V *norm) {
    V origin={dx,dy,0};float o[3],d[3];
    for(int j=0;j<3;j++) {o[j]=dot(origin,p->axis[j])/p->radius[j];d[j]=p->axis[j].z/p->radius[j];}
    bool found=false;
    for(int band=0;band<LAT;band++) {
        float lo=-1+2.0f*band/LAT,hi=-1+2.0f*(band+1)/LAT;
        float slope=bell_slopes[band],offset=bell_offsets[band];
        float r=slope*o[1]+offset,dr=slope*d[1];
        float a=d[0]*d[0]+d[2]*d[2]-dr*dr;
        float b=o[0]*d[0]+o[2]*d[2]-r*dr;
        float c=o[0]*o[0]+o[2]*o[2]-r*r;
        float roots[2];int nr=0;
        if(fabsf(a)<1e-7f) {if(fabsf(b)>1e-7f)roots[nr++]=-c/(2*b);}
        else {
            float disc=b*b-a*c;if(disc<0)continue;
            float sd=sqrtf(disc);roots[nr++]=(-b+sd)/a;roots[nr++]=(-b-sd)/a;
        }
        for(int k=0;k<nr;k++) {
            float z=roots[k],v=o[1]+d[1]*z;
            if(v<lo||v>hi||z+p->c.z<=*best)continue;
            float u=o[0]+d[0]*z,w=o[2]+d[2]*z;
            // A calla's spathe is asymmetrically open, exposing its spadix.
            if(p->shape==2&&(v>.28f-.8f*w||v>1-.65f*u*u))continue;
            V n=add(add(mul(p->axis[0],u/p->radius[0]),mul(p->axis[1],-(slope*v+offset)*slope/p->radius[1])),mul(p->axis[2],w/p->radius[2]));
            *best=z+p->c.z;*norm=n;found=true;
        }
    }
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
static void ray_row(uint16_t *row,int y) {
    // Preserve the woodland under overlapping petals during the dissolve.
    // Automatic storage only; no extra full-frame or persistent pixel buffer.
    uint16_t backdrop[FW];memcpy(backdrop,row+X0,sizeof backdrop);
    for(unsigned i=0;i<count;i++) {
        const Petal *p=&petals[i];if(y<p->ymin||y>p->ymax)continue;
        float dy=(65-(y+.5f))/SCALE-p->c.y;
        // PIE candidate (unmeasured): for ellipsoids, b, c and discriminant d
        // are polynomials across x. A bounded fixed-point 8-pixel rejection
        // pass could skip misses before scalar sqrt/depth/normal/shading.
        // PIE is not floating-point SIMD: retain conservative hit masks near
        // d=0, prove ranges, and compare silhouettes/depth with this reference.
        // Bell clipping is a separate path; measure it before extending this.
        for(int x=p->xmin;x<=p->xmax;x++) {
            float dx=(x+.5f-180)/SCALE-p->c.x;
            if(p->shape) {
                float z=depth[x-X0];V n;
                if(bell_hit(p,dx,dy,&z,&n)) {
                    depth[x-X0]=z;
                    row[x]=dissolve(backdrop[x-X0],shade(n,i,(V){dx+p->c.x,dy+p->c.y,z}));
                }
                continue;
            }
            float b=p->q[4]*dx+p->q[5]*dy;
            float c=p->q[0]*dx*dx+2*p->q[3]*dx*dy+p->q[1]*dy*dy-1;
            float d=b*b-p->q[2]*c;if(d<0)continue;
            float dz=(-b+sqrtf(d))*p->invzz,z=dz+p->c.z;
            if(z<=depth[x-X0])continue;
            depth[x-X0]=z;
            V n={p->q[0]*dx+p->q[3]*dy+p->q[4]*dz,
                 p->q[3]*dx+p->q[1]*dy+p->q[5]*dz,
                 p->q[4]*dx+p->q[5]*dy+p->q[2]*dz};
            row[x]=dissolve(backdrop[x-X0],shade(n,i,(V){dx+p->c.x,dy+p->c.y,z}));
        }
    }
}
void flower_draw(uint16_t *pixels,int y,int height) {
    if(!pixels||y<0||height<0||y>H||height>H-y)return;
    GardenFrame fallback;
    const GardenFrame *garden;
    if(seed_map)garden=(const GardenFrame*)(seed_map+32*32);
    else {garden_prepare(&fallback,elapsed);garden=&fallback;}
    for(int j=0;j<height;j++) {
        int py=y+j;uint16_t *row=pixels+j*W;
        garden_row(row,py,garden);
        // Without the block there is no flower, but there is still a sky. A
        // background that cannot allocate should look plain, not crash.
        if(py<12||py>119||!depth)continue;
        for(int x=0;x<FW;x++)depth[x]=-1000;
        ray_row(row,py);
    }
}
