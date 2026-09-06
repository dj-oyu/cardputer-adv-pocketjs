#include "solar_sail.h"
#include "solar_time.h"
#include "esp_cpu.h"
#include "esp_log.h"
#include <math.h>
#include <stdlib.h>
#include <stdbool.h>

// Temporary: where a frame's time goes, before anything is optimised for speed.
// PERF already separates prepare (prep) from draw (loop); this splits each of
// those into the parts that could be worked on independently.
static uint64_t t_orbits, t_scene, t_index, t_sky, t_lines;
static unsigned t_frames;
#define PHASE(acc) for(uint32_t _b=esp_cpu_get_cycle_count(), _o=1; _o; \
                       acc += esp_cpu_get_cycle_count()-_b, _o=0)

enum { W=240, H=135, MAX_LINES=1536, ORBIT_STEPS=96 };
#define RAD 0.017453292519943295f
typedef struct {float x,y,z;} Vec;
// y1=-1 encodes a disk with radius x1; all normal lines are viewport-clipped.
typedef struct {int16_t x0,y0,x1,y1;uint16_t color;} Line;
typedef struct {const char *name;double base[6],rate[6];uint8_t r,g,b;} Planet;
// JPL Table 1: a,e,I,L,varpi,Omega; AU/degrees, rates per Julian century.
// https://ssd.jpl.nasa.gov/planets/approx_pos.html (1800--2050 fit).
// EARTH is the Earth-Moon barycenter, adequate at this display scale.
static const Planet planets[8]={
 {"MERCURY",{.38709927,.20563593,7.00497902,252.25032350,77.45779628,48.33076593},{.00000037,.00001906,-.00594749,149472.67411175,.16047689,-.12534081},130,139,147},
 {"VENUS",{.72333566,.00677672,3.39467605,181.97909950,131.60246718,76.67984255},{.00000390,-.00004107,-.00078890,58517.81538729,.00268329,-.27769418},177,153,102},
 {"EARTH",{1.00000261,.01671123,-.00001531,100.46457166,102.93768193,0},{.00000562,-.00004392,-.01294668,35999.37244981,.32327364,0},63,145,192},
 {"MARS",{1.52371034,.09339410,1.84969142,-4.55343205,-23.94362959,49.55953891},{.00001847,.00007882,-.00813131,19140.30268499,.44441088,-.29257343},183,98,72},
 {"JUPITER",{5.202887,.04838624,1.30439695,34.39644051,14.72847983,100.47390909},{-.00011607,-.00013253,-.00183714,3034.74612775,.21252668,.20469106},172,145,115},
 {"SATURN",{9.53667594,.05386179,2.48599187,49.95424423,92.59887831,113.66242448},{-.00125060,-.00050991,.00193609,1222.49362201,-.41897216,-.28867794},178,160,111},
 {"URANUS",{19.18916464,.04725744,.77263783,313.23810451,170.95427630,74.01692503},{-.00196176,-.00004397,-.00242939,428.48202785,.40805281,.04240589},96,177,182},
 {"NEPTUNE",{30.06992276,.00859048,1.77004347,-55.12002969,44.96476227,131.78422574},{.00026291,.00005105,.00035372,218.45945325,-.32241464,-.00508664},70,109,193}
};
typedef struct {float a,e,b;Vec u,v,pos;} Orbit;
// JPL satellite mean elements at J2000: km, degrees, days.
// https://ssd.jpl.nasa.gov/sats/elem/ . These describe a mean ellipse, not
// an observation ephemeris; this lightweight model omits apsidal/nodal drift.
typedef struct {
    const char *name;unsigned parent;
    float a,e,w,m,inc,node;double period;
    float ra,dec;uint8_t r,g,b,size;
} Satellite;
static const Satellite satellites[]={
    {"MOON",2,384400,.0554f,318.15f,135.27f,5.16f,125.08f,27.322,0,0,166,178,188,4},
    {"IO",4,421800,.004f,49.1f,330.9f,0,0,1.762732,268.1f,64.5f,193,164,87,2},
    {"EUROPA",4,671100,.009f,45,345.4f,.5f,184,3.525463,268.1f,64.5f,150,182,192,2},
    {"GANYMEDE",4,1070400,.001f,198.3f,324.8f,.2f,58.5f,7.155588,268.2f,64.6f,158,144,126,3},
    {"CALLISTO",4,1882700,.007f,43.8f,87.4f,.3f,309.1f,16.690440,268.7f,64.8f,124,145,157,3},
    {"TITAN",5,1221900,.029f,78.3f,11.7f,.3f,78.6f,15.945448,36.4f,84,188,138,68,3}
};
#define SATELLITE_N (sizeof(satellites)/sizeof(satellites[0]))
static Orbit orbits[8];
static Orbit satellite_orbits[SATELLITE_N];
static Line lines[MAX_LINES];
enum { BANDS=(H+7)/8, INDEX_CAPACITY=4096 };
static uint16_t band_items[INDEX_CAPACITY],band_offsets[BANDS+1];
static bool index_valid,index_fits;
static uint8_t disk_half[25][25];
static unsigned count,focus;
static double elapsed,sim_days;
static solar_time_source_t time_source;
static float baseline_x,baseline_y,steer_x,steer_y;
static Vec right,up,front,center;
static float zoom,screen_x=178,cs[ORBIT_STEPS+1],sn[ORBIT_STEPS+1];
static Vec cage[11][33];
static bool ready;
static uint16_t sky[H];
static uint16_t rgb(int r,int g,int b){return (r>>3)<<11|(g>>2)<<5|(b>>3);}
static Vec add(Vec a,Vec b){return (Vec){a.x+b.x,a.y+b.y,a.z+b.z};}
static Vec mul(Vec a,float s){return (Vec){a.x*s,a.y*s,a.z*s};}
static float dot(Vec a,Vec b){return a.x*b.x+a.y*b.y+a.z*b.z;}
static Vec cross(Vec a,Vec b){return (Vec){a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};}
static Vec unit(Vec a){return mul(a,1/sqrtf(dot(a,a)));}
static Vec view(Vec a){return (Vec){dot(a,right),-dot(a,up),dot(a,front)};}
static Vec project(Vec a){Vec p=view(add(a,mul(center,-1)));return (Vec){screen_x+p.x*zoom,61+p.y*zoom,p.z};}
static float smooth(float x){return x*x*x*(10+x*(-15+6*x));}
// Kepler's equation. The mean anomaly arrives in double because it is a large
// angle -- Mercury's L passes 59,000 degrees over the demo's forty years -- and
// only double keeps the fractional part of that. Once it is folded to +-pi the
// value is small, and the rest of the solve runs in float: this core has a
// single-precision FPU, so every double sin and cos is a software call, and
// Newton was being asked for 1e-12 on a number that ends up as a pixel.
//
// Float's own limit is about 1e-7 rad, which at the widest zoom here is under a
// thousandth of a pixel. Three iterations reach it from this starting guess for
// every eccentricity in the tables (Mercury's 0.2056 is the largest).
static float eccentric(double m,float e) {
    float f=(float)remainder(m,6.283185307179586);
    float E=f+e*sinf(f);
    for(int j=0;j<4;j++) {
        float d=(E-e*sinf(E)-f)/(1-e*cosf(E));E-=d;
        if(fabsf(d)<1e-7f)break;
    }
    return E;
}
static Vec orbit_point(const Orbit *o,float c,float s) {
    return add(mul(o->u,o->a*(c-o->e)),mul(o->v,o->b*s));
}
static Orbit orbit_at(unsigned i,double days) {
    // centuries, once: the six elements were each dividing by 36525.
    double t=days/36525;
    double v[6];for(int j=0;j<6;j++)v[j]=planets[i].base[j]+planets[i].rate[j]*t;
    float inc=v[2]*RAD,w=(v[4]-v[5])*RAD,n=v[5]*RAD,e=v[1];
    float cw=cosf(w),sw=sinf(w),cn=cosf(n),ss=sinf(n),ci=cosf(inc),si=sinf(inc);
    Orbit o={.a=v[0],.e=e,.b=v[0]*sqrtf(1-e*e),
        .u={cw*cn-sw*ss*ci,cw*ss+sw*cn*ci,sw*si},
        .v={-sw*cn-cw*ss*ci,-sw*ss+cw*cn*ci,cw*si}};
    // The mean anomaly stays double until it is folded: L - varpi is the one
    // place where a large angle has to keep its fraction.
    float E=eccentric((v[3]-v[4])*0.017453292519943295,e);
    o.pos=orbit_point(&o,cosf(E),sinf(E));return o;
}
static Vec equatorial_to_ecliptic(Vec p) {
    const float e=23.43928f*RAD;
    return (Vec){p.x,p.y*cosf(e)+p.z*sinf(e),-p.y*sinf(e)+p.z*cosf(e)};
}
static Orbit satellite_shape(unsigned i) {
    const Satellite *s=&satellites[i];
    // Moon: ecliptic. Others: each satellite's own local Laplace plane,
    // whose origin is its ascending intersection with the ICRF equator.
    Vec x={1,0,0},y={0,1,0},z={0,0,1};
    if(i!=0) {
        float ra=s->ra*RAD,dec=s->dec*RAD;
        Vec axis={cosf(dec)*cosf(ra),cosf(dec)*sinf(ra),sinf(dec)};
        Vec node=unit(cross((Vec){0,0,1},axis));
        x=equatorial_to_ecliptic(node);
        y=equatorial_to_ecliptic(cross(axis,node));z=equatorial_to_ecliptic(axis);
    }
    float w=s->w*RAD,n=s->node*RAD,inc=s->inc*RAD;
    float cw=cosf(w),sw=sinf(w),cn=cosf(n),ss=sinf(n),ci=cosf(inc),si=sinf(inc);
    Orbit o={.a=s->a,.e=s->e,.b=s->a*sqrtf(1-s->e*s->e)};
    o.u=add(add(mul(x,cw*cn-sw*ss*ci),mul(y,cw*ss+sw*cn*ci)),mul(z,sw*si));
    o.v=add(add(mul(x,-sw*cn-cw*ss*ci),mul(y,-sw*ss+cw*cn*ci)),mul(z,cw*si));
    return o;
}
static Orbit satellite_at(unsigned i,double days) {
    // Mean element planes never change in this model. Keep the exact original
    // floating point results; do not repeat 6 plane transforms every frame.
    static Orbit shapes[SATELLITE_N];static bool shapes_ready;
    if(!shapes_ready) {
        for(unsigned j=0;j<SATELLITE_N;j++)shapes[j]=satellite_shape(j);
        shapes_ready=true;
    }
    const Satellite *s=&satellites[i];Orbit o=shapes[i];
    float E=eccentric(s->m*0.017453292519943295+6.283185307179586*days/s->period,s->e);
    o.pos=orbit_point(&o,cosf(E),sinf(E));return o;
}
// Clip before integer conversion: outer orbits are huge in an inner-planet view.
static bool clip(float p,float q,float *a,float *b) {
    if(fabsf(p)<1e-8f)return q>=0;
    float r=q/p;
    if(p<0){if(r>*b)return false;if(r>*a)*a=r;}
    else {if(r<*a)return false;if(r<*b)*b=r;}return true;
}
static void line(float x,float y,float xx,float yy,uint16_t color) {
    float dx=xx-x,dy=yy-y,a=0,b=1;
    if(!clip(-dx,x,&a,&b)||!clip(dx,W-1-x,&a,&b)||
       !clip(-dy,y,&a,&b)||!clip(dy,H-1-y,&a,&b)||count==MAX_LINES)return;
    lines[count++]=(Line){lroundf(x+a*dx),lroundf(y+a*dy),lroundf(x+b*dx),lroundf(y+b*dy),color};
    index_valid=false;
}
static void segment(Vec a,Vec b,uint16_t c){line(a.x,a.y,b.x,b.y,c);}
static void disk(Vec p,int r,uint16_t color) {
    if(p.x+r<0||p.x-r>=W||p.y+r<0||p.y-r>=H||count==MAX_LINES)return;
    lines[count++]=(Line){lroundf(p.x),lroundf(p.y),r,-1,color};
    index_valid=false;
}
static void line_bands(Line l,unsigned *first,unsigned *last) {
    int lo,hi;
    if(l.y1==-1){lo=l.y0-l.x1;hi=l.y0+l.x1;}
    else {lo=l.y0<l.y1?l.y0:l.y1;hi=l.y0>l.y1?l.y0:l.y1;}
    if(lo<0)lo=0;
    if(hi>=H)hi=H-1;
    *first=(unsigned)lo/8;*last=(unsigned)hi/8;
}
static void index_lines(void) {
    unsigned sizes[BANDS]={0};
    for(unsigned i=0;i<count;i++) {
        unsigned first,last;line_bands(lines[i],&first,&last);
        for(unsigned b=first;b<=last;b++)sizes[b]++;
    }
    unsigned total=0;band_offsets[0]=0;
    for(unsigned b=0;b<BANDS;b++){total+=sizes[b];band_offsets[b+1]=(uint16_t)total;}
    index_fits=total<=INDEX_CAPACITY;index_valid=true;
    if(!index_fits)return; // Exact full-scan fallback if a future scene grows.
    for(unsigned b=0;b<BANDS;b++)sizes[b]=band_offsets[b];
    // Stable order preserves all planet/ring/satellite occlusion semantics.
    for(unsigned i=0;i<count;i++) {
        unsigned first,last;line_bands(lines[i],&first,&last);
        for(unsigned b=first;b<=last;b++)band_items[sizes[b]++]=(uint16_t)i;
    }
}
// One linear km->pixel scale per satellite system: Jovian distance ratios
// remain intact. The parent disk and satellite sizes are enlarged separately.
static float satellite_scale(unsigned parent,float weight) {
    float extent=parent==2?54:parent==4?72:62;
    float km=parent==2?384400:parent==4?1882700:1221900;
    return extent*weight/km;
}
static Vec satellite_screen(unsigned i,Vec parent,float weight,Vec position) {
    Vec q=mul(view(position),satellite_scale(satellites[i].parent,weight));
    return (Vec){parent.x+q.x,parent.y+q.y,q.z};
}
static void satellite_paths(unsigned parent,Vec p,float weight) {
    if(weight<.05f)return;
    for(unsigned i=0;i<SATELLITE_N;i++)if(satellites[i].parent==parent) {
        const Orbit *o=&satellite_orbits[i];
        uint16_t c=rgb(12*weight,31*weight,43*weight);
        for(int k=0;k<ORBIT_STEPS;k+=4) {
            Vec a=satellite_screen(i,p,weight,orbit_point(o,cs[k],sn[k]));
            Vec b=satellite_screen(i,p,weight,orbit_point(o,cs[k+2],sn[k+2]));
            segment(a,b,c);
        }
    }
}
static void satellite_disks(unsigned parent,Vec p,float weight,bool near) {
    if(weight<.05f)return;
    Vec points[SATELLITE_N];unsigned order[SATELLITE_N],n=0;
    for(unsigned i=0;i<SATELLITE_N;i++)if(satellites[i].parent==parent) {
        Vec q=satellite_screen(i,p,weight,satellite_orbits[i].pos);
        if((q.z>=0)!=near)continue;
        points[i]=q;order[n++]=i;
    }
    for(unsigned i=1;i<n;i++)for(unsigned j=i;j>0&&points[order[j]].z<points[order[j-1]].z;j--){unsigned a=order[j];order[j]=order[j-1];order[j-1]=a;}
    for(unsigned j=0;j<n;j++) {
        unsigned i=order[j];const Satellite *s=&satellites[i];
        int radius=(int)lroundf(s->size*weight);if(radius<1)radius=1;
        float brightness=weight*(near?.9f:.65f);
        disk(points[i],radius,rgb(s->r*brightness,s->g*brightness,s->b*brightness));
    }
}
// J2000 IAU poles from NAIF pck00011.tpc, equatorial -> ecliptic.
// Fixed axes only: no surface longitude, precession or nutation animation.
// Mars uses the 2009 mean-pole pair also recorded in that kernel.
static Vec pole(unsigned i) {
    static const float poles[8][2]={{281.0103f,61.4155f},{272.76f,67.16f},
        {0,90},{317.68143f,52.88650f},{268.056595f,64.495303f},
        {40.589f,83.537f},{257.311f,-15.175f},{299.36f,43.46f}};
    float ra=poles[i][0]*RAD,dec=poles[i][1]*RAD;
    float x=cosf(dec)*cosf(ra),y=cosf(dec)*sinf(ra),z=sinf(dec),e=23.43928f*RAD;
    return (Vec){x,y*cosf(e)+z*sinf(e),-y*sinf(e)+z*cosf(e)};
}
static void ring(Vec p,float radius,Vec u,Vec v,bool near,uint16_t color) {
    for(int k=0;k<ORBIT_STEPS;k+=2) {
        Vec a=view(add(mul(u,cs[k]),mul(v,sn[k])));
        Vec b=view(add(mul(u,cs[k+2]),mul(v,sn[k+2])));
        if((a.z+b.z>=0)!=near)continue;
        line(p.x+a.x*radius,p.y+a.y*radius,p.x+b.x*radius,p.y+b.y*radius,color);
    }
}
static void globe(unsigned i,Vec p,float r) {
    const Planet *b=&planets[i];
    Vec axis=pole(i),u=unit(cross((Vec){1,0,0},axis)),v=cross(axis,u);
    bool rings=i==5;
    uint16_t ring_color=rgb(b->r/2,b->g/2,b->b/2);
    if(rings){ring(p,r*1.55f,u,v,false,ring_color);ring(p,r*2.15f,u,v,false,ring_color);}
    disk(p,(int)ceilf(r),rgb(3,7,14));
    Vec light=unit(mul(orbits[i].pos,-1));
    // Front hemisphere only; light follows the actual modeled Sun direction.
    for(int family=0;family<2;family++)for(int j=0;j<(family?6:5);j++) {
        Vec prev={0};float prev_front=-1;
        for(int k=0;k<=32;k++) {
            Vec n=cage[family?j+5:j][k];
            Vec normal=add(mul(axis,n.z),add(mul(u,n.x),mul(v,n.y)));
            Vec q=view(normal);float facing=q.z;
            Vec dest={p.x+r*q.x,p.y+r*q.y,0};
            float strength=.19f+.62f*fmaxf(0,dot(normal,light));
            if(k&&facing>=0&&prev_front>=0)
                segment(prev,dest,rgb(b->r*strength,b->g*strength,b->b*strength));
            prev=dest;prev_front=facing;
        }
    }
    for(int k=0;k<ORBIT_STEPS;k+=3)
        line(p.x+r*cs[k],p.y+r*sn[k],p.x+r*cs[k+3],p.y+r*sn[k+3],rgb(b->r/3,b->g/3,b->b/3));
    if(rings){ring(p,r*1.55f,u,v,true,ring_color);ring(p,r*2.15f,u,v,true,ring_color);}
}
const char *solar_sail_target(void){return planets[focus].name;}
const char *solar_sail_time_label(void){return time_source==SOLAR_TIME_UTC?"UTC":"DEMO";}
void solar_sail_prepare(float dt,int tx,int ty) {
    if(dt>0.1f)dt=.033f;
    if(dt<0)dt=0;
    elapsed+=dt;
    if(!ready) {
        for(int k=0;k<=ORBIT_STEPS;k++){cs[k]=cosf(k*6.2831853f/ORBIT_STEPS);sn[k]=sinf(k*6.2831853f/ORBIT_STEPS);}
        for(int j=0;j<11;j++)for(int k=0;k<=32;k++) {
            float lon=j>=5?(j-5)*6.2831853f/6:k*6.2831853f/32;
            float lat=j>=5?(-1.5707963f+k*3.1415926f/32):(j-2)*.48f;
            cage[j][k]=(Vec){cosf(lon)*cosf(lat),sinf(lon)*cosf(lat),sinf(lat)};
        }
        for(int y=0;y<H;y++)sky[y]=rgb(3,7+y/40,17+y/20);
        // r*r-dy*dy is a small exact integer; store the same floor(sqrtf()).
        for(int r=0;r<=24;r++)for(int dy=0;dy<=r;dy++)disk_half[r][dy]=(uint8_t)sqrtf(r*r-dy*dy);
        ready=true;
    }
    // Orbital epoch comes from the clock provider; elapsed only drives the
    // tour/IMU and unsynchronized demo. Resync never restarts the camera tour.
    solar_time_sample_t time=solar_time_now(elapsed);
    sim_days=time.days;time_source=time.source;
    PHASE(t_orbits) {
        for(unsigned i=0;i<8;i++)orbits[i]=orbit_at(i,sim_days);
        for(unsigned i=0;i<SATELLITE_N;i++)satellite_orbits[i]=satellite_at(i,sim_days);
    }
    float adapt=1-expf(-dt/3),follow=1-expf(-dt/.3f);
    baseline_x+=(tx-baseline_x)*adapt;baseline_y+=(ty-baseline_y)*adapt;
    steer_x+=((tx-baseline_x)/256-steer_x)*follow;steer_y+=((ty-baseline_y)/256-steer_y)*follow;
    double tour=fmod(elapsed,288);
    unsigned from=(unsigned)(tour/36),to=(from+1)%8;
    float local=(float)(tour-from*36),blend=local<=24?0:smooth((local-24)/12);
    focus=blend<.5f?from:to;
    screen_x=178-22*((from==4||from==5?1-blend:0)+(to==4||to==5?blend:0));
    center=add(mul(orbits[from].pos,1-blend),mul(orbits[to].pos,blend));
    float scale=expf(logf(orbits[from].a)*(1-blend)+logf(orbits[to].a)*blend);
    zoom=83/scale;
    float yaw=.38f+(float)sin(elapsed*.009)*.18f+steer_x*.48f;
    float elevation=.60f+steer_y*.38f;
    right=(Vec){cosf(yaw),sinf(yaw),0};
    front=(Vec){sinf(yaw)*cosf(elevation),-cosf(yaw)*cosf(elevation),sinf(elevation)};
    up=cross(front,right);
    count=0;index_valid=false;
    PHASE(t_scene) {
    for(unsigned i=0;i<36;i++) {
        unsigned seed=(i+1)*2654435761u;
        float x=(seed%W)-steer_x*6,y=((seed>>9)%H)+steer_y*4;
        line(x,y,x,y,rgb(25,38,54));
    }
    for(unsigned i=0;i<8;i++) {
        Vec a=project(orbit_point(&orbits[i],cs[0],sn[0]));
        for(int k=0;k<ORBIT_STEPS;k++) {
            Vec b=project(orbit_point(&orbits[i],cs[k+1],sn[k+1]));
            segment(a,b,i==focus?rgb(21,48,60):rgb(10,25,36));a=b;
        }
    }
    unsigned order[9];Vec positions[9];
    for(unsigned i=0;i<9;i++){order[i]=i;positions[i]=project(i==8?(Vec){0,0,0}:orbits[i].pos);}
    for(unsigned i=1;i<9;i++)for(unsigned j=i;j>0&&positions[order[j]].z<positions[order[j-1]].z;j--){unsigned a=order[j];order[j]=order[j-1];order[j-1]=a;}
    for(unsigned j=0;j<9;j++) {
        unsigned i=order[j];Vec p=positions[i];
        if(i==8){disk(p,5,rgb(127,82,32));disk(p,3,rgb(239,190,103));continue;}
        float weight=(i==from?1-blend:0)+(i==to?blend:0);
        float close_radius=i==2?20:i==4?11:i==5?18:24;
        float radius=2+(close_radius-2)*weight;
        float extent=fmaxf(radius*2.2f,76*weight);
        if(p.x+extent<0||p.x-extent>=W||p.y+extent<0||p.y-extent>=H)continue;
        satellite_paths(i,p,weight);
        satellite_disks(i,p,weight,false);
        if(weight>.05f)globe(i,p,radius);
        else disk(p,2,rgb(planets[i].r/2,planets[i].g/2,planets[i].b/2));
        satellite_disks(i,p,weight,true);
    }
    }
    PHASE(t_index) index_lines();

    if(++t_frames==60) {
        ESP_LOGI("solar","PHASES orbits=%.2f scene=%.2f index=%.2f | sky=%.2f lines=%.2f ms, %u segments",
                 t_orbits/60/240000.0, t_scene/60/240000.0, t_index/60/240000.0,
                 t_sky/60/240000.0, t_lines/60/240000.0, count);
        t_orbits=t_scene=t_index=t_sky=t_lines=0; t_frames=0;
    }
}
// Scalar-equivalent contiguous stores; no lookup, 16-byte aligned rows.
static void __attribute__((noinline)) fill_row(uint16_t *row,uint16_t color) {
#ifdef __XTENSA__
    int blocks=W/8;
    __asm__ volatile("ee.vldbc.16 q0, %[c]\n"
        "loopgtz %[n], 2f\n" "ee.vst.128.ip q0, %[out], 16\n" "2:\n"
        :[out] "+a"(row):[c] "a"(&color),[n] "a"(blocks):"memory");
#else
    for(int x=0;x<W;x++)row[x]=color;
#endif
}
void solar_sail_draw(uint16_t *pixels,int y,int height) {
    PHASE(t_sky) for(int j=0;j<height;j++)fill_row(pixels+j*W,sky[y+j]);
    bool indexed=y%8==0&&height==(H-y<8?H-y:8);
    if(indexed&&!index_valid)index_lines();
    indexed=indexed&&index_fits;
    unsigned begin=indexed?band_offsets[y/8]:0,end=indexed?band_offsets[y/8+1]:count;
    PHASE(t_lines)
    for(unsigned i=begin;i<end;i++) {
        Line l=lines[indexed?band_items[i]:i];
        if(l.y1==-1) {
            int r=l.x1;
            for(int py=y;py<y+height;py++) {
                int dy=py-l.y0;if(abs(dy)>r)continue;
                int half=r<=24?disk_half[r][abs(dy)]:(int)sqrtf(r*r-dy*dy);
                int left=l.x0-half,right_edge=l.x0+half;
                if(left<0)left=0;
                if(right_edge>=W)right_edge=W-1;
                for(int x=left;x<=right_edge;x++)pixels[(py-y)*W+x]=l.color;
            }
            continue;
        }
        if((l.y0<y&&l.y1<y)||(l.y0>=y+height&&l.y1>=y+height))continue;
        int x=l.x0,py=l.y0,dx=abs(l.x1-x),sx=x<l.x1?1:-1;
        int dy=-abs(l.y1-py),sy=py<l.y1?1:-1,err=dx+dy;
        for(;;) {
            if(py>=y&&py<y+height)pixels[(py-y)*W+x]=l.color;
            if(x==l.x1&&py==l.y1)break;
            int e=2*err;if(e>=dy){err+=dy;x+=sx;}if(e<=dx){err+=dx;py+=sy;}
        }
    }
}
