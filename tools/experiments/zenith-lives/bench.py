"""Natural real MEGADEMO loops; pure cached JS_Call and separate native present."""
import argparse,datetime,hashlib,json,os,random,statistics,subprocess
import common
R=common.R;once=common.once
PRELUDE=r'''
static const char PRELUDE[]=
 "globalThis.console={log:__log};globalThis.pocket={kasane:kasane,memory:{info(){return {internalFreeBytes:31000-1000*__mega.live}}}};"
 "(()=>{const P=pocket.kasane.procedural,R=P.register,U=P.unregister;const M=globalThis.__mega={live:0,total:0,peak:0,frameReg:0,frameUnreg:0};"
 "P.register=function(){const h=R.apply(P,arguments);M.live++;M.total++;M.frameReg++;M.peak=Math.max(M.peak,M.live);return h};"
 "P.unregister=function(h){U.call(P,h);M.live--;M.frameUnreg++}})();";
'''
HELPER=r'''
static uint64_t mp_clock(clockid_t id){struct timespec t;clock_gettime(id,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static JSValue mp_fn,mp_stats;
static JSAtom mp_reg_atom,mp_unreg_atom;
static uint64_t mp_hash=1469598103934665603ull;
static unsigned mp_index,mp_loop,mp_lengths[6];
static struct {uint64_t wall,cpu,present;size_t calls,bytes,rise;unsigned scene,reg,buttons;} mp_rows[2048];
static unsigned mp_rows_n;
static size_t mp_frame_peak;
static int mp_tick(bool record,unsigned buttons){
 JS_SetProperty(ctx,mp_stats,mp_reg_atom,JS_NewInt32(ctx,0));JS_SetProperty(ctx,mp_stats,mp_unreg_atom,JS_NewInt32(ctx,0));
 JSValue button=JS_NewInt32(ctx,buttons);size_t calls=n_alloc,bytes=b_alloc,base=cur_bytes;turn_hi=base;
 uint64_t cpu=mp_clock(CLOCK_THREAD_CPUTIME_ID),start=mp_clock(CLOCK_MONOTONIC);
 JSValue v=JS_Call(ctx,mp_fn,JS_UNDEFINED,1,&button);
 uint64_t wall=mp_clock(CLOCK_MONOTONIC)-start,cpu_time=mp_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
 if(JS_IsException(v)){JS_FreeValue(ctx,v);return 3;}JS_FreeValue(ctx,v);
 size_t used_calls=n_alloc-calls,used_bytes=b_alloc-bytes,rise=turn_hi-base;
 if(record&&turn_hi>mp_frame_peak)mp_frame_peak=turn_hi;
 JSContext *jobs;while(JS_ExecutePendingJob(rt,&jobs)>0){}pocket_kasane_end_turn();
 unsigned index=mp_index++%mp_loop,scene=0;while(scene<5&&index>=mp_lengths[scene])index-=mp_lengths[scene++];
 JSValue rv=JS_GetProperty(ctx,mp_stats,mp_reg_atom);uint32_t reg=0;JS_ToUint32(ctx,&reg,rv);JS_FreeValue(ctx,rv);
 start=mp_clock(CLOCK_MONOTONIC);present((uint64_t)mp_index*33333u);uint64_t present_time=mp_clock(CLOCK_MONOTONIC)-start;
 if(record){mp_rows[mp_rows_n++]=(typeof(mp_rows[0])){wall,cpu_time,present_time,used_calls,used_bytes,rise,scene,reg,buttons};}
 for(size_t i=0;i<sizeof panel;i++){mp_hash^=((unsigned char*)panel)[i];mp_hash*=1099511628211ull;}
 const char *fd=getenv("ZENITH_PANEL_FD");if(fd){const unsigned char *p=(unsigned char*)panel;size_t left=sizeof panel;while(left){ssize_t n=write(atoi(fd),p,left);if(n<=0)abort();left-=n;p+=n;}}
 return exceptions||degrades||present_failures?4:0;
}
static int mp_run(size_t eval_peak){
 JSValue g=JS_GetGlobalObject(ctx);mp_fn=JS_GetPropertyStr(ctx,g,"frame");mp_stats=JS_GetPropertyStr(ctx,g,"__mega");
 JSValue app=JS_GetPropertyStr(ctx,g,"procMegademo"),len=JS_GetPropertyStr(ctx,app,"len");
 for(unsigned i=0;i<6;i++){JSValue v=JS_GetPropertyUint32(ctx,len,i);JS_ToUint32(ctx,&mp_lengths[i],v);JS_FreeValue(ctx,v);mp_loop+=mp_lengths[i];}
 JS_FreeValue(ctx,len);JS_FreeValue(ctx,app);JS_FreeValue(ctx,g);
 mp_reg_atom=JS_NewAtom(ctx,"frameReg");mp_unreg_atom=JS_NewAtom(ctx,"frameUnreg");
 if(!mp_loop||mp_loop>2048)return 5;
 for(unsigned i=0;i<mp_loop;i++)if(mp_tick(false,0))return 6;
 JS_RunGC(rt);size_t before=cur_bytes,prior_peak=peak_bytes;peak_bytes=cur_bytes;
 JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);
 for(unsigned i=0;i<mp_loop*3;i++)if(mp_tick(true,0))return 7;
 size_t window_peak=peak_bytes,whole_peak=prior_peak>window_peak?prior_peak:window_peak;
 JS_RunGC(rt);size_t retained=cur_bytes;
 printf("ZENITH_RESULT {\"pointer_bytes\":%zu,\"loop\":%u,\"hash\":\"%016llx\",\"log\":\"%s\",\"eval_peak\":%zu,\"retained_before\":%zu,\"retained_after\":%zu,\"window_peak\":%zu,\"whole_peak\":%zu,\"frame_live_peak\":%zu,\"bytecode\":%lld,\"samples\":[",sizeof(void*),mp_loop,(unsigned long long)mp_hash,scene_log,eval_peak,before,retained,window_peak,whole_peak,mp_frame_peak,(long long)m.js_func_code_size);
 for(unsigned i=0;i<mp_rows_n;i++){typeof(mp_rows[0])*r=&mp_rows[i];printf("%s[%llu,%llu,%llu,%zu,%zu,%zu,%u,%u,%u]",i?",":"",(unsigned long long)r->wall,(unsigned long long)r->cpu,(unsigned long long)r->present,r->calls,r->bytes,r->rise,r->scene,r->reg,r->buttons);}
 puts("]}");JS_FreeAtom(ctx,mp_reg_atom);JS_FreeAtom(ctx,mp_unreg_atom);JS_FreeValue(ctx,mp_fn);JS_FreeValue(ctx,mp_stats);
 pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);return 0;
}

'''
def prepare(out,m32=False,transform=None):
 apps=common.inputs(out);s=(R/'tools/kasane_contract/test_megademo_app_host.c').read_text()
 s=once(s,'#include <time.h>','#include <time.h>\n#include <unistd.h>')
 s=once(s,'static size_t cur_bytes,peak_bytes;','static size_t cur_bytes,peak_bytes,n_alloc,b_alloc,turn_hi;')
 s=once(s,'cur_bytes+=add;cur_bytes-=sub;','cur_bytes+=add;cur_bytes-=sub;if(add){n_alloc++;b_alloc+=add;}if(cur_bytes>turn_hi)turn_hi=cur_bytes;')
 start=s.index('static const char PRELUDE[]=');end=s.index('static JSValue js_now',start);s=s[:start]+PRELUDE+'\n'+s[end:]
 s=once(s,'if(getenv("MEGA_LOG")||strncmp(s,"MEGADEMO SCENE",14))','if(getenv("ZENITH_LOG"))')
 s=once(s,'int main(int argc,char **argv){',HELPER+'\nint main(int argc,char **argv){')
 s=once(s,'    /* Buttons: two plain loops,','    if(!ok)return 1;\n    return mp_run(eval_peak);\n    /* Buttons: two plain loops,')
 h=out/'perf.c';h.write_text(transform(s) if transform else s);return apps,common.build(out/'build',h,m32=m32),h

def run(binary,app,m32=False,fd=None):
 env={k:v for k,v in os.environ.items() if not k.startswith(('MEGA_','ZENITH_'))}
 env['MEGA_HEAP_LIMIT']='163840' if m32 else '524288'
 if fd is not None:env['ZENITH_PANEL_FD']=str(fd)
 text=subprocess.check_output([str(binary),str(app)],cwd=R,env=env,text=True,pass_fds=(() if fd is None else (fd,)))
 return json.loads(next(l.removeprefix('ZENITH_RESULT ') for l in text.splitlines() if l.startswith('ZENITH_RESULT ')))

def main():
 p=argparse.ArgumentParser();p.add_argument('--pairs',type=int,default=15);p.add_argument('--m32',action='store_true');a=p.parse_args()
 out=R/('.cache/zenith-lives-m32' if a.m32 else '.cache/zenith-lives');apps,binary,h=prepare(out,a.m32)
 if hasattr(os,'sched_getaffinity'):os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
 dest=out/('bench-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));dest.mkdir();rows=[]
 for pair in range(-2,a.pairs):
  checked={}
  for order in range(2):
   mode=('baseline','candidate')[(pair+order)&1];d=run(binary,apps[mode],m32=a.m32);checked[mode]=d
   if pair>=0:rows.append({**d,'pair':pair,'mode':mode,'order':order})
  assert all(checked['baseline'][k]==checked['candidate'][k] for k in ('hash','loop','log','pointer_bytes'))
 summary={'heap_limit':163840 if a.m32 else 524288,'initial_gc_threshold_clamp':81920 if a.m32 else 262144,'base':common.BASE,'local_head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=R,text=True).strip(),'pairs':a.pairs,'inputs':{m:hashlib.sha256(p.read_bytes()).hexdigest() for m,p in apps.items()},'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'harness_sha256':hashlib.sha256(h.read_bytes()).hexdigest(),'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],'sample_fields':['js_wall_ns','js_cpu_ns','present_wall_ns','successful_allocations','charged_bytes','peak_rise','scene','registrations','buttons'],'groups':{}}
 for name,scene in [('all',None),('NEWS',0),('TWIST',3),('ZENITH',4),('LIMIT',5)]:
  data={m:[{**r,'samples':[v for v in r['samples'] if scene is None or (v[6]==scene if scene else v[6]<3)]} for r in rows if r['mode']==m] for m in apps};g={}
  for m,rs in data.items():
   vs=[v for r in rs for v in r['samples']];g[m]={'mean_js_wall_ns':statistics.median(statistics.mean(v[0] for v in r['samples']) for r in rs),'mean_js_cpu_ns':statistics.median(statistics.mean(v[1] for v in r['samples']) for r in rs),'mean_present_ns':statistics.median(statistics.mean(v[2] for v in r['samples']) for r in rs),'allocations':statistics.mean(v[3] for v in vs),'charged_bytes':statistics.mean(v[4] for v in vs),'frames':len(rs[0]['samples']),**{k:sorted(set(r[k] for r in rs)) for k in ('eval_peak','retained_before','retained_after','window_peak','whole_peak','frame_live_peak','bytecode')}}
  for label,col in [('wall',0),('cpu',1),('js_plus_present',None)]:
   ratios=[sum(v[0]+v[2] if col is None else v[col] for v in data['candidate'][i]['samples'])/sum(v[0]+v[2] if col is None else v[col] for v in data['baseline'][i]['samples']) for i in range(a.pairs)];rng=random.Random(423);bs=sorted(statistics.median(rng.choices(ratios,k=len(ratios))) for _ in range(5000));g[label+'_ratio']=statistics.median(ratios);g[label+'_bootstrap95']=[bs[124],bs[4874]]
  summary['groups'][name]=g
 (dest/'raw.json').write_text(json.dumps(rows,separators=(',',':'))+'\n');(dest/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({'out':str(dest),**summary},indent=2))
if __name__=='__main__':main()
