"""Build a same-process host comparison; never treats host ms as device ms."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'main/scene/garden.c').read_text()
section = source[source.index('typedef struct { unsigned seed,phase,tick;'):source.index('static void garden_vegetation_row(')]
# Reconstruct the immediately preceding, broad-volume version from this task.
prior = section.replace('.radius=18+', '.radius=10+')
prior = prior.replace('#if GARDEN_DECOR_EVENTS', '#if 0')
prior = prior.replace('int radius=(decor.radius+y/8)*256+(cloud-128)*6;', 'int radius=(decor.radius+y/10)*256;')
prior = prior.replace('(half/2-6)', '(half/2)')
prior = prior.replace('((light*(r+6))>>1)', '(light*5)').replace('((light*(g+10))>>2)', '(light*7)').replace('((light*(b+4))>>2)', '(light*2)')
prior = prior.replace('garden_decor', 'prior_decor').replace('GardenDecor', 'PriorDecor')
noevent = section.replace('#if GARDEN_DECOR_EVENTS', '#if 0').replace('garden_decor', 'noevent_decor').replace('GardenDecor', 'NoeventDecor')
counted = section.replace('garden_decor', 'count_decor').replace('GardenDecor', 'CountDecor')
counted = counted.replace('if(y>=end)continue;', 'if(y>=end)continue;\n        setups++;')
counted = counted.replace('for(int x=lo;x<=hi;x++) {', 'for(int x=lo;x<=hi;x++) { visits++;')
counted = counted.replace('if(gap<=0)continue;', 'if(gap<=0)continue;\n            profiles++;')
counted = counted.replace('if(!light&&!shadow)continue;', 'if(!light&&!shadow)continue;\n            blends++;')
prior_counted = counted.replace('.radius=18+', '.radius=10+').replace('int radius=(decor.radius+y/8)*256+(cloud-128)*6;', 'int radius=(decor.radius+y/10)*256;').replace('(half/2-6)', '(half/2)')
prior_counted = prior_counted.replace('#if GARDEN_DECOR_EVENTS', '#if 0')
prior_counted = prior_counted.replace('count_decor', 'prior_count_decor').replace('CountDecor', 'PriorCountDecor')
harness = r'''
#include "../main/scene/garden.c"
#include <stdio.h>
#include <time.h>
static unsigned long long setups,visits,profiles,blends;
'''+prior+noevent+counted+prior_counted+r'''
static uint16_t row[240];
static volatile unsigned sink;
typedef void (*Draw)(uint16_t*,int,const GardenFrame*);
static double bench(Draw draw) {
    clock_t t=clock();
    for(int pass=0;pass<32;pass++)for(int frame=0;frame<256;frame++) {
        GardenFrame f={0};garden_prepare(&f,frame*.5f);
        for(int y=0;y<135;y++) {
            for(int x=0;x<240;x++)row[x]=0x2987;
            if(draw)draw(row,y,&f);
            sink+=row[(frame+y)%240];
        }
    }
    return (double)(clock()-t)*1000/CLOCKS_PER_SEC/(32*256);
}
int main(void) {
    for(int frame=0;frame<256;frame++) {
        GardenFrame f={0};garden_prepare(&f,frame*.5f);
        for(int y=0;y<135;y++)prior_count_decor_row(row,y,&f);
    }
    printf("PRIOR avg setups=%.1f visits=%.1f profiles=%.1f blends=%.1f\n",setups/256.,visits/256.,profiles/256.,blends/256.);
    setups=visits=profiles=blends=0;
    unsigned long long peakv=0,peakp=0,peakb=0,peaks=0;int peakframe=0;
    for(int frame=0;frame<256;frame++) {
        GardenFrame f={0};garden_prepare(&f,frame*.5f);
        unsigned long long ov=visits,op=profiles,ob=blends,os=setups;
        for(int y=0;y<135;y++) {for(int x=0;x<240;x++)row[x]=0x2987;count_decor_row(row,y,&f);}
        if(blends-ob>peakb){peakv=visits-ov;peakp=profiles-op;peakb=blends-ob;peaks=setups-os;peakframe=frame;}
    }
    printf("CURRENT avg setups=%.1f visits=%.1f profiles=%.1f blends=%.1f\n",setups/256.,visits/256.,profiles/256.,blends/256.);
    printf("CURRENT busiest-blend-frame t=%.1f setups=%llu visits=%llu profiles=%llu blends=%llu\n",peakframe*.5,peaks,peakv,peakp,peakb);
    for(int trial=0;trial<3;trial++) {
        double base=bench(NULL),old=bench(prior_decor_row),plain=bench(noevent_decor_row),now=bench(garden_decor_row);
        printf("HOST trial=%d baseline=%.6f prior=%.6f wide-noevents=%.6f current=%.6f ms/frame events-ratio=%.3f total-ratio=%.3f\n",trial,base,old-base,plain-base,now-base,(now-base)/(plain-base),(now-base)/(old-base));
    }
}
'''
out = root / '.cache/bench_decor_cost.c'
out.write_text(harness)
gcc = 'C:/msys64/ucrt64/bin/gcc.exe'
exe = root / '.cache/bench_decor_cost.exe'
subprocess.run([gcc, '-O2', '-Wall', '-Wextra', '-Wno-unused-function', str(out), '-lm', '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True, cwd=root)
