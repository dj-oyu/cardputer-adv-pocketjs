#!/usr/bin/env python3
"""Real-app A/B for the mixed-frame plain horizontal raster path. No USB."""
import argparse,hashlib,json,os,random,re,shutil,statistics,subprocess,time
from pathlib import Path
import run_derby
ROOT=Path(__file__).resolve().parents[2]
BASE='e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267'
SOURCE='main/ui/kasane/ksn_procedural.c'
def replace(s,a,b):
    assert s.count(a)==1,a
    return s.replace(a,b)
WRAPPER=r'''
#include <stdio.h>
#include <time.h>
int hline_scope,hline_trace;
unsigned long long hline_calls,hline_ext,hline_hits,hline_ns;
static FILE *hline_bands;
static unsigned long long hline_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000+t.tv_nsec;}
__attribute__((destructor)) static void hline_close(void){if(hline_bands)fclose(hline_bands);}
bool ksn_proc_render_band(const ksn_proc_frame *f,uint16_t *p,int y,int height){
    if(!hline_scope)return hline_render_real(f,p,y,height);
    unsigned hits=0;
    if(hline_trace&&f&&f->ext){
        for(unsigned j=0;j<f->count;j++){
            const ksn_proc_segment *s=&f->segments[j];
            int lo=s->y0<s->y1?s->y0:s->y1,hi=s->y0>s->y1?s->y0:s->y1;
            if(hi<y||lo>=y+height)continue;
            if(s->x0>=KSN_PROC_EXT_MIN){j++;continue;}
            hits+=s->y0==s->y1;
        }
    }
    const unsigned long long start=hline_now();
    bool ok=hline_render_real(f,p,y,height);
    const unsigned long long elapsed=hline_now()-start;
    hline_calls++;hline_ext+=f&&f->ext;hline_hits+=hits;hline_ns+=elapsed;
    if(hline_trace){
        if(!hline_bands)hline_bands=fopen(getenv("HLINE_BANDS"),"w");
        if(!hline_bands)abort();
        fprintf(hline_bands,"%llu,%u,%u\n",elapsed,f&&f->ext,hits);
    }
    return ok;
}
'''
def harness(out):
    s=(ROOT/'tools/games/test_derby_host.c').read_text()
    s=replace(s,'/* ---- the procedural oracle */',r'''
extern int hline_scope,hline_trace;
extern unsigned long long hline_calls,hline_ext,hline_hits,hline_ns;
static FILE *hline_frames;
static unsigned long long hline_js_start,hline_js_ns,hline_alloc,hline_bytes;
static unsigned long long hline_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000+t.tv_nsec;}
static uint64_t hline_panel_hash=1469598103934665603ull;
__attribute__((destructor)) static void hline_close(void){
    if(hline_frames)fclose(hline_frames);
    printf("HLINE_PRESENT calls=%llu ext=%llu hits=%llu ns=%llu panel=%016llx\n",hline_calls,hline_ext,hline_hits,hline_ns,(unsigned long long)hline_panel_hash);
}
/* ---- the procedural oracle */''')
    s=replace(s,'static void present(uint64_t now_us){','''static void present(uint64_t now_us){
    const unsigned long long count=hline_calls,ns=hline_ns;
    hline_trace=getenv("HLINE_BANDS")!=NULL;
    hline_scope=1;const unsigned long long started=hline_now();''')
    s=replace(s,'    else pocket_kasane_animations_presented(now_us);','''    else pocket_kasane_animations_presented(now_us);
    const unsigned long long elapsed=hline_now()-started;hline_scope=0;
    if(!hline_frames){hline_frames=fopen(getenv("HLINE_FRAMES"),"w");REQ(hline_frames);}
    fprintf(hline_frames,"%u,%s,%llu,%llu,%llu,%llu,%llu,%llu\\n",tick,scene,elapsed,hline_calls-count,hline_ns-ns,hline_js_ns,hline_alloc,hline_bytes);
    if(getenv("HLINE_CAPTURE"))for(unsigned i=0;i<sizeof panel;i++){hline_panel_hash^=((unsigned char*)panel)[i];hline_panel_hash*=1099511628211ull;}''')
    s=replace(s,'churn_base=turn_hi=cur_bytes;return JS_UNDEFINED;','churn_base=turn_hi=cur_bytes;hline_js_start=hline_now();return JS_UNDEFINED;')
    s=replace(s,'    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){','''    hline_js_ns=hline_now()-hline_js_start;hline_alloc=n_alloc-churn_n;hline_bytes=b_alloc-churn_b;
    for(unsigned i=0;i<5;i++)if(!strcmp(scene,CHURN_SCENE[i])){''')
    p=out/'harness.c';p.write_text(s);return p

def summarize(rows):
    summaries={};rng=random.Random(42)
    for case in sorted({r['case'] for r in rows}):
        pairs=[]
        for n in sorted({r['pair'] for r in rows if r['case']==case and r['pair']>0}):
            pairs.append(sorted((r for r in rows if r['case']==case and r['pair']==n),key=lambda r:r['mode']))
        summaries[case]={}
        for key in ['present_ns_median','present_ns_total','band_ns_total','diagnostic_frame_ns_total','wall_s']:
            if not pairs:continue
            ratios=[b[key]/a[key] for a,b in pairs]
            boot=sorted(statistics.median(rng.choices(ratios,k=len(ratios))) for _ in range(10000))
            summaries[case][key]=dict(pairs=len(pairs),base=statistics.median(a[key] for a,b in pairs),candidate=statistics.median(b[key] for a,b in pairs),paired_ratio=statistics.median(ratios),bootstrap95=[boot[249],boot[9749]])
    return summaries

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--pairs',type=int,default=9);p.add_argument('--sanitize',action='store_true');p.add_argument('--out',type=Path,default=ROOT/'.cache/hline')
    p.add_argument('--cases',default='light,oval,straight',help='light,oval,straight,mid; mid is the full lifecycle script')
    a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
    if hasattr(os,'sched_getaffinity'):os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
    flags=['-O2']+(['-fsanitize=address,undefined','-fno-pie','-no-pie'] if a.sanitize else [])
    sources={'base':subprocess.check_output(['git','show',BASE+':'+SOURCE],cwd=ROOT,text=True),'fast':(ROOT/SOURCE).read_text()}
    original_sources=run_derby.kasane_sources();bins={};h=harness(out)
    for mode,source in sources.items():
        folder=out/mode;folder.mkdir(exist_ok=True)
        q=folder/'ksn_procedural.c';q.write_text(replace(source,'bool ksn_proc_render_band(const ksn_proc_frame *frame,','bool hline_render_real(const ksn_proc_frame *frame,')+WRAPPER)
        run_derby.kasane_sources=lambda:[str(q) if x==SOURCE else x for x in original_sources]
        if mode=='fast':
            (folder/'build').mkdir(exist_ok=True)
            for obj in (out/'base/build').glob('*.o'):shutil.copy(obj,folder/'build'/obj.name)
        bins[mode]=run_derby.build(flags,folder/'build',harness=h)
    app=run_derby.lower(ROOT/'apps/derby',out/'app');apps={'light':app,'mid':app}
    for c in ['oval','straight']:apps[c]=run_derby.force_course(run_derby.lower(ROOT/'apps/derby',out/c),c)
    rows=[]
    for case in a.cases.split(','):
        for pair in range(a.pairs+1):
            for mode in (('base','fast') if pair%2==0 else ('fast','base')):
                prefix=out/mode/f'{case}-{pair}'
                env=dict(os.environ,DERBY_APP_DIR=str(apps[case]),DERBY_TIER='0' if case=='light' else '1' if case=='mid' else '2',DERBY_HEAP_LIMIT='524288',HLINE_FRAMES=str(prefix)+'.frames',ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1')
                if pair==0:env.update(HLINE_CAPTURE='1',HLINE_BANDS=str(prefix)+'.bands',DERBY_CSV=str(prefix)+'.csv')
                t=time.perf_counter();run=subprocess.run([str(bins[mode])],cwd=ROOT,env=env,capture_output=True,text=True);elapsed=time.perf_counter()-t
                prefix.with_suffix('.log').write_text(run.stdout+run.stderr)
                assert run.returncode==0 and 'DERBY_HOST PASS' in run.stdout,str(prefix)
                f=[x.split(',') for x in prefix.with_suffix('.frames').read_text().splitlines()]
                present=[int(x[2]) for x in f[1:]];bands=[int(x[4]) for x in f[1:]];js=[int(x[5]) for x in f[1:]]
                counts=list(map(int,re.search(r'HLINE_PRESENT calls=(\d+) ext=(\d+) hits=(\d+) ns=(\d+)',run.stdout).groups()))
                item=dict(mode=mode,case=case,pair=pair,frames=len(present),present_ns_total=sum(present),present_ns_median=statistics.median(present),band_ns_total=sum(bands),diagnostic_frame_ns_total=sum(x+y for x,y in zip(js,present)),wall_s=elapsed,counts=counts,allocations=sum(int(x[6]) for x in f[1:]),allocated_bytes=sum(int(x[7]) for x in f[1:]),pixels=re.search(r'pixels hash (\w+)',run.stdout)[1],panel=re.search(r'panel=(\w+)',run.stdout)[1],finish=re.search(r'^finish: (.+)',run.stdout,re.M)[1])
                rows.append(item);print(json.dumps(item),flush=True)
            base,fast=sorted(rows[-2:],key=lambda x:x['mode'])
            for k in ['frames','pixels','panel','finish','allocations','allocated_bytes']:assert base[k]==fast[k],(case,pair,k)
            assert base['counts'][:3]==fast['counts'][:3]
            if pair==0:assert (out/'base'/f'{case}-0.csv').read_bytes()==(out/'fast'/f'{case}-0.csv').read_bytes()
    assert (ROOT/SOURCE).read_text()==sources['fast']
    report=dict(base=BASE,head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),candidate_sha256=hashlib.sha256(sources['fast'].encode()).hexdigest(),flags=flags,compiler=subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],scope='actual pocket_kasane_present only for band/present timers; diagnostic frame includes host oracle. pair0 classifies hits/captures and is excluded from timing.',rows=rows,summary=summarize(rows))
    (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print('HLINE_APP PASS')
if __name__=='__main__':main()
