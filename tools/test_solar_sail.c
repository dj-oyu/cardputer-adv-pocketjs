// Host check: cc -O2 -fsanitize=address,undefined tools/test_solar_sail.c -lm -o /tmp/test-sail
//
// scene_mem.c comes first because both of the others draw from it: the scene's
// 29,579 bytes of arrays now live in two borrowed blocks rather than in .bss,
// and the block contract is checked at the end of main().
#include "../main/scene/scene_mem.c"
#include "../main/scene/solar_sail.c"
#include "../main/scene/solar_time.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

// A second scene, borrowing the same core and bulk and leaving them full of
// rubbish. Its address is all it needs to be one.
static const char foreign_owner;
static void foreign_scribble(void) {
    bool rebuild;
    void *core=scene_mem(&foreign_owner,sizeof(solar_core_t),&rebuild);
    assert(core&&rebuild);
    memset(core,0x5a,sizeof(solar_core_t));
    void *bulk=scene_bulk(&foreign_owner,sizeof(solar_bulk_t),&rebuild);
    assert(bulk&&rebuild);
    memset(bulk,0x5a,sizeof(solar_bulk_t));
}
// The scene drifts: elapsed, the tilt baseline and the steering carry from
// frame to frame. Drawing the same pose twice means setting all of it and then
// asking for a zero-length frame, so nothing advances between the two.
static void pose(double at) {
    elapsed=at;baseline_x=baseline_y=steer_x=steer_y=0;
    solar_sail_prepare(0,120,-90);
}
int main(int argc,char **argv) {
    // Kepler residual across the full anomaly range, including Mercury's e.
    //
    // The bound is float's, not double's. eccentric() folds the mean anomaly in
    // double -- Mercury's L passes 59,000 degrees over the tour and only double
    // keeps the fraction of that -- and then solves in float, because this core
    // has a single-precision FPU and the answer ends up as a pixel. Measured
    // worst case over this sweep is 2.48e-7 rad at Mercury, -226 deg, which is
    // float's own limit and, as the note beside eccentric() says, under a
    // thousandth of a pixel at the widest zoom. 5e-7 still catches a solver
    // that stops converging; 1e-10 would be asserting the arithmetic is double.
    for(int i=0;i<8;i++)for(int j=-360;j<=360;j++) {
        double m=j*0.017453292519943295,e=planets[i].base[1];
        double E=eccentric(m,e);
        assert(fabs(remainder(E-e*sin(E)-m,6.283185307179586))<5e-7);
    }
    for(int i=0;i<8;i++)for(int day=0;day<=14610;day+=487) {
        Orbit o=orbit_at(i,day);
        assert(fabsf(dot(o.u,o.u)-1)<1e-5f&&fabsf(dot(o.u,o.v))<1e-5f);
        float radius=sqrtf(dot(o.pos,o.pos));
        assert(radius>=o.a*(1-o.e)-1e-4f&&radius<=o.a*(1+o.e)+1e-4f);
        Vec peri=orbit_point(&o,1,0),apo=orbit_point(&o,-1,0);
        assert(fabsf(sqrtf(dot(peri,peri))-o.a*(1-o.e))<1e-4f);
        assert(fabsf(sqrtf(dot(apo,apo))-o.a*(1+o.e))<1e-4f);
    }
    uint16_t full[W*H],assembled[W*H];
    unsigned per_parent[8]={0};
    for(unsigned i=0;i<SATELLITE_N;i++) {
        const Satellite *s=&satellites[i];per_parent[s->parent]++;
        for(int step=0;step<32;step++) {
            double day=step*s->period/32;
            Orbit a=satellite_at(i,day),b=satellite_at(i,day+s->period);
            Vec diff=add(a.pos,mul(b.pos,-1));
            assert(sqrtf(dot(diff,diff))/s->a<1e-5f);
            float distance=sqrtf(dot(a.pos,a.pos));
            assert(distance>=s->a*(1-s->e)-1&&distance<=s->a*(1+s->e)+1);
            assert(fabsf(dot(a.u,a.v))<1e-5f&&fabsf(dot(a.u,a.u)-1)<1e-5f);
        }
    }
    assert(per_parent[2]==1&&per_parent[4]==4&&per_parent[5]==1&&SATELLITE_N==6);
    struct {uint16_t guard[8],data[W*8],end[8];} band;
    unsigned peak_lines=0;bool index_overflowed=false;
    for(int frame=0;frame<1200;frame++) {
        elapsed=frame*.25; // All eight visits, transitions, and wraparound.
        int tx=frame<300?0:frame<600?180:frame<900?-180:0;
        int ty=frame<600?0:frame<900?180:-180;
        solar_sail_prepare(1.0f/30,tx,ty);
        // MAX_LINES sizes the larger half of the bulk block, so how close the
        // tour actually comes to it is the number that says whether 1,536 can
        // be cut. Reported below rather than left as a bound nobody has read.
        assert(count<MAX_LINES);
        if(count>peak_lines)peak_lines=count;
        if(!index_fits)index_overflowed=true;
        solar_sail_draw(full,0,H);
        for(int y=0;y<H;y+=8) {
            memset(&band,0xa5,sizeof(band));
            int h=H-y<8?H-y:8;
            solar_sail_draw(band.data,y,h);
            for(int i=0;i<8;i++)assert(band.guard[i]==0xa5a5&&band.end[i]==0xa5a5);
            for(int i=W*h;i<W*8;i++)assert(band.data[i]==0xa5a5);
            memcpy(assembled+y*W,band.data,W*h*2);
        }
        assert(memcmp(full,assembled,sizeof(full))==0);
    }
    // Smooth center and magnification at every stop / departure / loop join.
    for(int i=0;i<=8;i++)for(int j=0;j<2;j++) {
        double t=i*36+(j?24:0);if(t==0)continue;
        elapsed=t-0.0001;solar_sail_prepare(0,0,0);Vec a=center;float z=zoom;
        elapsed=t+0.0001;solar_sail_prepare(0,0,0);
        assert(sqrtf(dot(add(a,mul(center,-1)),add(a,mul(center,-1))))<.001f);
        assert(fabsf(z-zoom)<.01f);
    }
    // Force a lunar occultation and transit through the same screen pixel.
    elapsed=84;solar_sail_prepare(0,0,0);
    for(int near=0;near<=1;near++) {
        count=0;Vec p={100,70,0};
        satellite_orbits[0].pos=mul(front,near?384400:-384400);
        satellite_disks(2,p,1,false);globe(2,p,20);satellite_disks(2,p,1,true);
        solar_sail_draw(full,0,H);
        const Satellite *s=&satellites[0];float brightness=near?.9f:.65f;
        uint16_t moon=rgb(s->r*brightness,s->g*brightness,s->b*brightness);
        if(near)assert(full[70*W+100]==moon);
        else assert(full[70*W+100]!=moon);
    }
    if(argc>1) {
        static uint16_t tiles[W*H*8];
        for(int planet=0;planet<8;planet++) {
            elapsed=planet*36+12;baseline_x=baseline_y=steer_x=steer_y=0;
            solar_sail_prepare(0,0,0);assert(focus==(unsigned)planet);
            solar_sail_draw(full,0,H);
            for(int y=0;y<H;y++)memcpy(tiles+(planet/4*H+y)*W*4+planet%4*W,full+y*W,W*2);
        }
        FILE *f=fopen(argv[1],"wb");assert(f);fprintf(f,"P6\n%d %d\n255\n",W*4,H*2);
        for(int i=0;i<W*H*8;i++) {
            unsigned v=tiles[i];
            unsigned char c[]={((v>>11)&31)*255/31,((v>>5)&63)*255/63,(v&31)*255/31};
            fwrite(c,1,3,f);
        }
        fclose(f);
    }
    // ---- the two-tier scene block -------------------------------------
    // The core (trigonometry, sky gradient, disk table) and the bulk (the
    // display list and its band index) are borrowed from scene_mem, which
    // hands the same address to another scene and grows it without warning.
    // A pose drawn on a released block, and on one a foreign owner has
    // scribbled over, must equal the pose drawn on a block nobody touched --
    // that is the whole of the contract, and a cache flag kept outside the
    // block is the one way to fail it.
    static uint16_t reference[W*H];
    pose(37);solar_sail_draw(reference,0,H);

    // Note the order: a release is followed by a prepare, never by a draw.
    // main.c releases in begin_run() and shell_draw calls prepare before its
    // strip loop, so the firmware never draws on a block it has not just
    // re-borrowed. The pointers here would be stale if it did.
    scene_mem_release();
    pose(37);solar_sail_draw(full,0,H);
    assert(memcmp(reference,full,sizeof(full))==0);

    foreign_scribble();
    pose(37);solar_sail_draw(full,0,H);
    assert(memcmp(reference,full,sizeof(full))==0);
    // Strips too: this is the path that reads band_items, the half of the bulk
    // a whole-frame call never touches.
    for(int y=0;y<H;y+=8) {
        int h=H-y<8?H-y:8;
        solar_sail_draw(assembled+y*W,y,h);
    }
    assert(memcmp(reference,assembled,sizeof(full))==0);

    // Drawing without a block. The bulk gone but the core kept is a sky with
    // no solar system on it; neither one is a dereference.
    scene_mem_release();
    pose(37);
    lines=NULL;band_items=NULL;count=0;
    solar_sail_draw(full,0,H);
    for(int i=0;i<W*H;i++)assert(full[i]==sky[i/W]);
    sky=NULL;
    solar_sail_draw(full,0,H);
    for(int i=0;i<W*H;i++)assert(full[i]==rgb(3,7,17));
    scene_mem_release();

    printf("SOLAR_SAIL_OK 6 satellites, periods, occultation/transit, Kepler "
           "residual, full tour, 1200 frames, strips and bounds; block "
           "recycling and both null paths; peak %u lines of %u%s; core %zu "
           "bulk %zu, static residue %zu bytes\n",
           peak_lines,(unsigned)MAX_LINES,
           index_overflowed?" (band index overflowed)":"",
           sizeof(solar_core_t),sizeof(solar_bulk_t),
           sizeof(orbits)+sizeof(satellite_orbits)+sizeof(band_offsets));
}
