#!/usr/bin/env python3
"""Independent BIG WAVE curlAt section window evidence; host only, no device operations."""
import argparse,csv,hashlib,json,os,random,re,shutil,statistics,subprocess,time
from pathlib import Path
import run_big_wave as bw
ROOT=bw.ROOT;BASE='e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267';SOURCE='apps/bigwave/big_wave.js';HEAP_LIMIT=524288
def replace(s,a,b):
 assert s.count(a)==1,a
 return s.replace(a,b)
def harness(out):
 s=(ROOT/'tools/games/test_big_wave_host.c').read_text()
 s=replace(s,'#include <string.h>','#include <string.h>\n#include <time.h>')
 s=replace(s,'static size_t cur_bytes,peak_bytes;','static size_t cur_bytes,peak_bytes,curl_alloc,curl_bytes;\nstatic FILE *curl_csv,*curl_panels,*curl_draws,*curl_states;\nstatic bool curl_verify;\nstatic JSValue curl_fn;\nstatic unsigned long long curl_now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (unsigned long long)t.tv_sec*1000000000+t.tv_nsec;}')
 s=replace(s,'h->n=n;pk_count(tlsf_len(n),0);','h->n=n;curl_alloc++;curl_bytes+=tlsf_len(n);pk_count(tlsf_len(n),0);')
 s=replace(s,'q->n=n;pk_count(tlsf_len(n),tlsf_len(old));','q->n=n;curl_alloc++;curl_bytes+=tlsf_len(n);pk_count(tlsf_len(n),tlsf_len(old));')
 s=replace(s,'    else pocket_kasane_animations_presented(now);','    else pocket_kasane_animations_presented(now);\n    if(curl_panels)fwrite(panel,sizeof panel,1,curl_panels);')
 s=replace(s,'    rec_committed=false;rec_n=0;','''    if(curl_draws){
        unsigned head[]={frame_no,rec_committed,rec_n};fwrite(head,sizeof head,1,curl_draws);
        for(unsigned i=0;i<rec_n;i++){fwrite(&rec[i].h,sizeof(rec[i].h),1,curl_draws);fwrite(&rec[i].n,sizeof(rec[i].n),1,curl_draws);fwrite(rec[i].in,sizeof(float),rec[i].n,curl_draws);}
    }
    rec_committed=false;rec_n=0;''')
 start=s.index('static void frame(uint32_t buttons){');end=s.index('static void frames(',start)
 s=s[:start]+'''static int curl_state(void){
    if(!curl_states)return 1;
    const char *expr="JSON.stringify(bigWave.S)";
    JSValue v=JS_Eval(ctx,expr,strlen(expr),"state.js",JS_EVAL_TYPE_GLOBAL);CHECK(!JS_IsException(v));
    const char *str=JS_ToCString(ctx,v);CHECK(str);fprintf(curl_states,"%s\\n",str);JS_FreeCString(ctx,str);JS_FreeValue(ctx,v);return 1;
}
static void frame(uint32_t buttons){
    frame_no++;unsigned long long outer=curl_now();pocket_input_pump(buttons);
    JSValue arg=JS_NewInt32(ctx,buttons);size_t a0=curl_alloc,b0=curl_bytes;
    unsigned long long t=curl_now();JSValue result=JS_Call(ctx,curl_fn,JS_UNDEFINED,1,&arg);unsigned long long js=curl_now()-t;
    size_t da=curl_alloc-a0,db=curl_bytes-b0;
    if(JS_IsException(result)){exceptions++;JSValue e=JS_GetException(ctx);const char *str=JS_ToCString(ctx,e);fprintf(stderr,"frame exception: %s\\n",str?str:"?");if(str)JS_FreeCString(ctx,str);JS_FreeValue(ctx,e);}JS_FreeValue(ctx,result);
    JSContext *job;while(JS_ExecutePendingJob(rt,&job)>0){}pocket_kasane_end_turn();
    if(curl_verify)oracle();t=curl_now();present();unsigned long long present_ns=curl_now()-t,all=curl_now()-outer;
    if(curl_csv)fprintf(curl_csv,"%u,%llu,%llu,%llu,%zu,%zu,%zu\\n",frame_no,js,present_ns,all,da,db,cur_bytes);
    curl_state();size_t h=heap_used();if(h>heap_peak)heap_peak=h;
    if(curl_verify && !(frame_no%16)){JS_RunGC(rt);h=heap_used();if(h>heap_live_peak)heap_live_peak=h;}
}
''' +s[end:]
 s=replace(s,'int main(int argc,char **argv){',r'''static JSValue curl_values(JSContext *c,JSValueConst self,int argc,JSValueConst *argv){
    (void)self;if(argc!=1)abort();JSValue length=JS_GetPropertyStr(c,argv[0],"length");uint32_t n=0;if(JS_ToUint32(c,&n,length)!=0)abort();JS_FreeValue(c,length);
    printf("CURL_VALUES");for(uint32_t i=0;i<n;i++){JSValue v=JS_GetPropertyUint32(c,argv[0],i);double d;if(JS_ToFloat64(c,&d,v)!=0)abort();JS_FreeValue(c,v);printf(" %a",d);}puts("");return JS_UNDEFINED;
}
int main(int argc,char **argv){''')
 s=replace(s,'    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));','    JS_SetPropertyStr(ctx,g,"__curl_values",JS_NewCFunction(ctx,curl_values,"values",1));\n    JS_SetPropertyStr(ctx,g,"__log",JS_NewCFunction(ctx,js_log,"log",1));')
 s=replace(s,'    ppm_dir=getenv("BW_PPM");','''    ppm_dir=getenv("BW_PPM");curl_verify=getenv("CURL_VERIFY")!=NULL;
    if(getenv("CURL_PANELS"))curl_panels=fopen(getenv("CURL_PANELS"),"wb");
    if(getenv("CURL_DRAWS"))curl_draws=fopen(getenv("CURL_DRAWS"),"wb");
    if(getenv("CURL_STATES"))curl_states=fopen(getenv("CURL_STATES"),"w");
    if(getenv("CURL_CSV")){curl_csv=fopen(getenv("CURL_CSV"),"w");fprintf(curl_csv,"frame,js_ns,present_ns,owner_present_ns,alloc,bytes,live\\n");}''')
 minimal='globalThis.console={log:globalThis.__log};globalThis.pocket={kasane:globalThis.kasane,input:globalThis.input,capabilities:{get:n=>({name:n,supported:__cap(n)||n==="audio.tone",available:true})},audio:{tone:globalThis.__tone}};(function(){const P=pocket.kasane.procedural,R=P.register,U=P.unregister;P.register=function(c,p){const h=p?R.call(P,c,p):R.call(P,c);__reg(h,c,p||null);return h};P.unregister=function(h){U.call(P,h);__unreg(h)}})();'
 s=replace(s,'    eval(PRELUDE,strlen(PRELUDE),"prelude.js");','    if(curl_verify)eval(PRELUDE,strlen(PRELUDE),"prelude.js");else {const char *s='+json.dumps(minimal)+';eval(s,strlen(s),"minimal.js");}')
 s=replace(s,'    const size_t eval_peak=peak_bytes;','    const size_t eval_peak=peak_bytes;\n    {JSValue g=JS_GetGlobalObject(ctx);curl_fn=JS_GetPropertyStr(ctx,g,"frame");JS_FreeValue(ctx,g);}')
 s=replace(s,'    if(!ok)return 1;','''    if(!ok)return 1;
    if(getenv("CURL_CASE")||getenv("CURL_FIXTURE")){
        frames(40,"");
        if(getenv("CURL_CASE")&&!strcmp(getenv("CURL_CASE"),"ride")){frames(1,"1");frames(1,"");}
        if(getenv("CURL_CASE")&&!strcmp(getenv("CURL_CASE"),"pause")){frames(1,"1");frames(1,"");frames(1,"tab");frames(1,"");}
        unsigned begin=frame_no;size_t live_before=cur_bytes;
        if(getenv("CURL_PROBE"))run("globalThis.__ca=globalThis.__sa=0");
        if(getenv("CURL_FIXTURE")){
            FILE *f=fopen(getenv("CURL_FIXTURE"),"r");if(!f)return 2;char line[8192];
            while(fgets(line,sizeof line,f)){frame_no++;run(line);oracle();present();curl_state();}
            fclose(f);
        }else frames(120,"");
        if(getenv("CURL_PROBE"))printf("CURL_REACH curlAt=%.0f secAmp=%.0f\\n",num("__ca"),num("__sa"));
        printf("CURL_RESULT begin=%u end=%u eval_live=%zu eval_peak=%zu live_before=%zu live_after=%zu peak=%zu exceptions=%u present=%u oracle=%u\\n",begin,frame_no,heap_after_eval,eval_peak,live_before,cur_bytes,peak_bytes,exceptions,present_failures,oracle_failures);
        bool pass=!exceptions&&!present_failures&&!oracle_failures;
        if(curl_csv)fclose(curl_csv);if(curl_panels)fclose(curl_panels);if(curl_draws)fclose(curl_draws);if(curl_states)fclose(curl_states);
        JS_FreeValue(ctx,curl_fn);pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);return pass?0:1;
    }''')
 s=replace(s,'    pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);','    JS_FreeValue(ctx,curl_fn);pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);') if s.count('    pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);')==1 else s
 p=out/'harness.c';p.write_text(s);return p
def build(out,sanitize=False,m32=False):
 cache=out/'build';cache.mkdir(parents=True,exist_ok=True)
 flags=['-O1','-fsanitize=address,undefined'] if sanitize else ['-O2']
 if m32:
  flags+=(os.environ['M32_CFLAGS'].split() if os.environ.get('M32_CFLAGS') else subprocess.check_output(['bash','tools/vmtest/m32_sysroot.sh'],cwd=ROOT,env=dict(os.environ,M32_SYSROOT=str(ROOT/'.cache/m32sys')),text=True).split())
 (out/'build-flags.json').write_text(json.dumps(flags)+'\n')
 subprocess.run(['python3','tools/make_font.py',str(cache)],cwd=ROOT,check=True,stdout=subprocess.DEVNULL)
 data=(ROOT/'apps/pet/assets/pets-compact.bin').read_bytes();(cache/'kasane_pet_test_data.h').write_text('static const uint8_t pet_test_data[] = {'+','.join(map(str,data))+'};\n')
 api=(ROOT/'main/pocket/pocket_api.c').read_text();(cache/'pocket_sub_impl.inc').write_text(api[api.index('// ----------------------------------------------------------- subscriptions'):api.index('// ------------------------------------------------------- async completions')])
 objs=[]
 for name in ('dtoa','libregexp','libunicode','quickjs','quickjs-vm'):
  obj=cache/(name+'.o');src=ROOT/bw.QJS/(name+'.c')
  if not obj.exists():subprocess.run(['gcc','-std=gnu11','-c','-g','-w',*flags,'-DQUICKJS_NG_BUILD','-D_GNU_SOURCE','-I'+bw.QJS,'-Icomponents/pocketjs_guest/include',str(src),'-o',str(obj)],cwd=ROOT,check=True)
  objs.append(str(obj))
 binary=cache/'curl';h=harness(out)
 includes=['-I'+p for p in (bw.QJS,'tools/hostshim','main','main/pocket','main/ui','main/ui/kasane','main/text','main/hal',str(cache))]
 subprocess.run(['gcc','-std=gnu11','-g',*flags,'-DKSN_PROC_POINTS_PIE_MODEL','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fno-omit-frame-pointer',*includes,str(h),*bw.kasane_sources(),*bw.EXTRA,*objs,'-Wl,--wrap=calloc','-Wl,--wrap=free','-lm','-o',str(binary)],cwd=ROOT,check=True)
 return binary
def run(binary,source,prefix,case=None,verify=False,probe=False,fixture=None):
 env=dict(os.environ,BW_HEAP_LIMIT=str(HEAP_LIMIT),ASAN_OPTIONS='detect_leaks=0:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1',CURL_CSV=str(prefix)+'.csv')
 if case:env['CURL_CASE']=case
 if verify:env.update(CURL_VERIFY='1',CURL_PANELS=str(prefix)+'.panels',CURL_DRAWS=str(prefix)+'.draws',CURL_STATES=str(prefix)+'.states')
 if probe:env['CURL_PROBE']='1'
 if fixture:env['CURL_FIXTURE']=str(fixture)
 t=time.perf_counter();r=subprocess.run([str(binary),str(source)],cwd=ROOT,env=env,capture_output=True,text=True);wall=time.perf_counter()-t
 prefix.with_suffix('.log').write_text(r.stdout+r.stderr);assert r.returncode==0,str(prefix)
 with prefix.with_suffix('.csv').open() as f:rows=list(csv.DictReader(f))
 if case:rows=rows[-120:]
 totals={k:sum(int(x[k]) for x in rows) for k in ('js_ns','present_ns','owner_present_ns','alloc','bytes')}
 return {'wall':wall,'totals':totals,'result':re.search(r'CURL_RESULT .*',r.stdout)[0] if case or fixture else re.search(r'frames \d+:.*',r.stdout)[0]}
from curl_boundary import boundaries

def ratio(pairs,key):
 values=[p['fast']['totals'][key]/p['base']['totals'][key] for p in pairs]
 rng=random.Random(417);boots=sorted(statistics.median(rng.choices(values,k=len(values))) for _ in range(10000))
 return {'median':statistics.median(values),'bootstrap95':[boots[249],boots[9749]],'ratios':values}
def main():
 global HEAP_LIMIT
 p=argparse.ArgumentParser();p.add_argument('--heap-limit',type=int,default=524288);p.add_argument('--m32',action='store_true');p.add_argument('--binary',type=Path);p.add_argument('--out',type=Path,required=True);p.add_argument('--pairs',type=int,default=9);p.add_argument('--sanitize',action='store_true');p.add_argument('--verify-only',action='store_true');p.add_argument('--timing-only',action='store_true');a=p.parse_args();out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
 HEAP_LIMIT=a.heap_limit
 if hasattr(os,'sched_getaffinity'):os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
 base=subprocess.check_output(['git','show',BASE+':'+SOURCE],cwd=ROOT,text=True);fast=(ROOT/SOURCE).read_text()
 for mode,s in [('base',base),('fast',fast)]: (out/(mode+'.js')).write_text(s)
 binary=a.binary.resolve() if a.binary else build(out,a.sanitize,a.m32);result={'heap_limit':a.heap_limit,'m32':a.m32,'base':BASE,'candidate_sha256':hashlib.sha256(fast.encode()).hexdigest()}
 if not a.timing_only:
  for mode in ('base','fast'):result[mode]=run(binary,out/(mode+'.js'),out/('verify-'+mode),verify=True)
  for ext in ('panels','draws','states'):
   x=(out/('verify-base.'+ext)).read_bytes();y=(out/('verify-fast.'+ext)).read_bytes();assert x==y,ext
   result[ext]={'bytes':len(x),'sha256':hashlib.sha256(x).hexdigest()}
  result['panels']['count']=result['panels']['bytes']//64800
  result['boundary']=boundaries(binary,out,base,fast,run,replace)
  print(json.dumps({'compatibility':result}),flush=True)
 if not a.verify_only:
  for case in ('title','ride','pause'):
   pairs=[]
   for i in range(a.pairs):
    pair={}
    for mode in (('base','fast') if i%2==0 else ('fast','base')):pair[mode]=run(binary,out/(mode+'.js'),out/(f'{case}-{i}-{mode}'),case=case)
    for k in ('alloc','bytes'):assert pair['base']['totals'][k]==pair['fast']['totals'][k],(case,i,k)
    pairs.append(pair);print(json.dumps({'case':case,'pair':i,**pair}),flush=True)
   result[case]={'pairs':pairs,'summary':{k:ratio(pairs,k) for k in ('js_ns','present_ns','owner_present_ns')}}
 (out/'result.json').write_text(json.dumps(result,indent=2)+'\n');print('CURL_SUMMARY '+json.dumps({k:v['summary'] for k,v in result.items() if isinstance(v,dict) and 'summary' in v}),flush=True);print('CURL_DEEP PASS',flush=True)
if __name__=='__main__':main()
