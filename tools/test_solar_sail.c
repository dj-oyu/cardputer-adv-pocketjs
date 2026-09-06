// Host check: cc -O2 -fsanitize=address,undefined tools/test_solar_sail.c -lm -o /tmp/test-sail
#include "../main/solar_sail.c"
#include "../main/solar_time.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(int argc,char **argv) {
    // Kepler residual across the full anomaly range, including Mercury's e.
    for(int i=0;i<8;i++)for(int j=-360;j<=360;j++) {
        double m=j*0.017453292519943295,e=planets[i].base[1];
        double E=eccentric(m,e);
        assert(fabs(remainder(E-e*sin(E)-m,6.283185307179586))<1e-10);
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
    for(int frame=0;frame<1200;frame++) {
        elapsed=frame*.25; // All eight visits, transitions, and wraparound.
        int tx=frame<300?0:frame<600?180:frame<900?-180:0;
        int ty=frame<600?0:frame<900?180:-180;
        solar_sail_prepare(1.0f/30,tx,ty);
        assert(count<MAX_LINES);
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
    puts("SOLAR_SAIL_OK 6 satellites, periods, occultation/transit, Kepler residual, full tour, 1200 frames, strips and bounds");
}
