"""Actual LCD CATCH JS_Call plus separate present, native A/D or natural input."""
import argparse,datetime,hashlib,json,os,random,statistics,subprocess
import common
R=common.R;once=common.once
HELPER=r'''
static JSValue lc_fn,lc_state;static JSAtom lc_mode,lc_score;
static uint64_t lc_hash=1469598103934665603ull;
static bool lc_input,lc_bot;static unsigned lc_tick_n;
static struct {uint64_t wall,cpu,present;size_t calls,bytes,rise;int32_t mode,score;} lc_rows[120];
static size_t lc_frame_peak;
static uint64_t lc_clock(clockid_t id){struct timespec t;clock_gettime(id,&t);return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
static int lc_tick(int index){
 if(lc_bot){int lane=(int)num(BOT),pl=(int)num("lcdCatch.pl");const char *key=lane>=0&&lane<pl?"a":lane>pl?"d":NULL;if(key){edge(key,true);edge(key,false);}}
 if(lc_input){const char *key=lc_tick_n++&1?"d":"a";edge(key,true);edge(key,false);}pocket_input_pump(0);
 JSValue b=JS_NewInt32(ctx,0);size_t calls=n_alloc,bytes=b_alloc,base=cur_bytes;turn_hi=base;
 uint64_t cpu=lc_clock(CLOCK_THREAD_CPUTIME_ID),start=lc_clock(CLOCK_MONOTONIC);
 JSValue v=JS_Call(ctx,lc_fn,JS_UNDEFINED,1,&b);
 uint64_t wall=lc_clock(CLOCK_MONOTONIC)-start,dt=lc_clock(CLOCK_THREAD_CPUTIME_ID)-cpu;
 if(JS_IsException(v)){JS_FreeValue(ctx,v);return 3;}JS_FreeValue(ctx,v);
 size_t nc=n_alloc-calls,nb=b_alloc-bytes,rise=turn_hi-base;if(index>=0&&turn_hi>lc_frame_peak)lc_frame_peak=turn_hi;
 JSContext *jobs;while(JS_ExecutePendingJob(rt,&jobs)>0){}pocket_kasane_end_turn();
 size_t pc=n_alloc,pb=b_alloc;int32_t mode,score;v=JS_GetProperty(ctx,lc_state,lc_mode);JS_ToInt32(ctx,&mode,v);JS_FreeValue(ctx,v);v=JS_GetProperty(ctx,lc_state,lc_score);JS_ToInt32(ctx,&score,v);JS_FreeValue(ctx,v);if(pc!=n_alloc||pb!=b_alloc)return 4;
 start=lc_clock(CLOCK_MONOTONIC);present();uint64_t pr=lc_clock(CLOCK_MONOTONIC)-start;
 if(index>=0){lc_rows[index]=(typeof(lc_rows[0])){wall,dt,pr,nc,nb,rise,mode,score};for(size_t i=0;i<sizeof panel;i++){lc_hash^=((unsigned char*)panel)[i];lc_hash*=1099511628211ull;}}
 return exceptions||bad_present?5:0;
}
static int lc_run(const char *kind){
 open_session();frames(30);
 if(strcmp(kind,"title"))tap("1");
 lc_input=!strcmp(kind,"input");lc_bot=!strcmp(kind,"bot");
 JSValue g=JS_GetGlobalObject(ctx);lc_fn=JS_GetPropertyStr(ctx,g,"frame");lc_state=JS_GetPropertyStr(ctx,g,"lcdCatch");JS_FreeValue(ctx,g);lc_mode=JS_NewAtom(ctx,"mode");lc_score=JS_NewAtom(ctx,"score");
 for(int i=0;i<40;i++)if(lc_tick(-1))return 6;
 JS_RunGC(rt);size_t before=cur_bytes,prior_peak=peak_bytes;peak_bytes=cur_bytes;JSMemoryUsage m;JS_ComputeMemoryUsage(rt,&m);
 for(int i=0;i<120;i++)if(lc_tick(i))return 7;
 size_t window_peak=peak_bytes,whole_peak=prior_peak>window_peak?prior_peak:window_peak;JS_RunGC(rt);size_t retained=cur_bytes;
 printf("LCD7_RESULT {\"case\":\"%s\",\"pointer_bytes\":%zu,\"hash\":\"%016llx\",\"heap_eval\":%zu,\"heap_eval_peak_delta\":%zu,\"retained_before\":%zu,\"retained_after\":%zu,\"window_peak\":%zu,\"whole_peak\":%zu,\"frame_live_peak\":%zu,\"bytecode\":%lld,\"samples\":[",kind,sizeof(void*),(unsigned long long)lc_hash,heap_eval,heap_eval_peak,before,retained,window_peak,whole_peak,lc_frame_peak,(long long)m.js_func_code_size);
 for(unsigned i=0;i<120;i++){typeof(lc_rows[0])*r=&lc_rows[i];printf("%s[%llu,%llu,%llu,%zu,%zu,%zu,%d,%d]",i?",":"",(unsigned long long)r->wall,(unsigned long long)r->cpu,(unsigned long long)r->present,r->calls,r->bytes,r->rise,r->mode,r->score);}puts("]}");
 JS_FreeAtom(ctx,lc_mode);JS_FreeAtom(ctx,lc_score);JS_FreeValue(ctx,lc_fn);JS_FreeValue(ctx,lc_state);close_session();return 0;
}
'''
def prepare(out,m32=False):
 apps=common.inputs(out);s=(R/'tools/games/test_lcd_catch_host.c').read_text()
 s=once(s,'#include <string.h>','#include <string.h>\n#include <time.h>')
 s=once(s,'static size_t cur_bytes,peak_bytes;','static size_t cur_bytes,peak_bytes,n_alloc,b_alloc,turn_hi;')
 s=once(s,'cur_bytes+=add;cur_bytes-=sub;','cur_bytes+=add;cur_bytes-=sub;if(add){n_alloc++;b_alloc+=add;}if(cur_bytes>turn_hi)turn_hi=cur_bytes;')
 s=once(s,'    printf("  %s\\n",s);','    if(getenv("LCD7_LOG"))printf("  %s\\n",s);')
 s=once(s,'int main(void){',HELPER+'\nint main(int argc,char **argv){')
 s=once(s,'FILE *f=fopen("apps/lcdcatch/lcd_catch.js","rb");','FILE *f=fopen(argc>1?argv[1]:"apps/lcdcatch/lcd_catch.js","rb");')
 s=once(s,'    printf("session 1: GAME A, pause, game over, GAME B, Back\\n");','    if(getenv("LCD7_CASE"))return lc_run(getenv("LCD7_CASE"));\n    printf("session 1: GAME A, pause, game over, GAME B, Back\\n");')
 h=out/'perf.c';h.write_text(s);return apps,common.build(out/'build',h,m32=m32),h

def main():
 p=argparse.ArgumentParser();p.add_argument('--pairs',type=int,default=40);p.add_argument('--m32',action='store_true');a=p.parse_args();out=R/('.cache/lcd7-m32' if a.m32 else '.cache/lcd7');apps,binary,h=prepare(out,a.m32)
 if hasattr(os,'sched_getaffinity'):os.sched_setaffinity(0,{min(os.sched_getaffinity(0))})
 dest=out/('bench-'+datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ'));dest.mkdir();rows=[]
 for case in ('natural','input','bot','title'):
  for pair in range(-2,a.pairs):
   check={}
   for order in range(2):
    mode=('baseline','candidate')[(pair+order)&1];env={**{k:v for k,v in os.environ.items() if not k.startswith(('LCD_','LCD7_'))},'LCD7_CASE':case}
    text=subprocess.check_output([str(binary),str(apps[mode])],cwd=R,env=env,text=True);d=json.loads(next(l.removeprefix('LCD7_RESULT ') for l in text.splitlines() if l.startswith('LCD7_RESULT ')));check[mode]=d
    if pair>=0:rows.append({**d,'mode':mode,'pair':pair,'order':order})
   assert check['baseline']['hash']==check['candidate']['hash'];assert [v[6:] for v in check['baseline']['samples']]==[v[6:] for v in check['candidate']['samples']]
 summary={'base':common.BASE,'source_commit':'d09525422d9f404f84e84f022daec95597e12018','pairs':a.pairs,'source_hashes':{m:hashlib.sha256(p.read_bytes()).hexdigest() for m,p in apps.items()},'binary_sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'harness_sha256':hashlib.sha256(h.read_bytes()).hexdigest(),'compiler':subprocess.check_output(['gcc','--version'],text=True).splitlines()[0],'sample_fields':['js_wall_ns','js_cpu_ns','present_ns','successful_allocations','charged_bytes','peak_rise','mode','score'],'groups':{}}
 for case in ('natural','input','bot','title'):
  rs={m:[r for r in rows if r['case']==case and r['mode']==m] for m in apps};g={}
  for m,data in rs.items():
   vs=[v for r in data for v in r['samples']];g[m]={'median_mean_js_ns':statistics.median(statistics.mean(v[0] for v in r['samples']) for r in data),'median_mean_present_ns':statistics.median(statistics.mean(v[2] for v in r['samples']) for r in data),'mean_successful_allocations':statistics.mean(v[3] for v in vs),'mean_charged_bytes':statistics.mean(v[4] for v in vs),'max_peak_rise':max(v[5] for v in vs),'mode_frames':{str(mode):sum(v[6]==mode for v in data[0]['samples']) for mode in range(5)},**{k:sorted(set(r[k] for r in data)) for k in ('heap_eval','heap_eval_peak_delta','retained_before','retained_after','window_peak','whole_peak','frame_live_peak','bytecode')}}
  for label,col in [('wall',0),('cpu',1)]:
   ratios=[sum(v[col] for v in rs['candidate'][i]['samples'])/sum(v[col] for v in rs['baseline'][i]['samples']) for i in range(a.pairs)];rng=random.Random(423);bs=sorted(statistics.median(rng.choices(ratios,k=len(ratios))) for _ in range(5000));g[label+'_ratio']=statistics.median(ratios);g[label+'_bootstrap95']=[bs[124],bs[4874]]
  summary['groups'][case]=g
 (dest/'raw.json').write_text(json.dumps(rows,separators=(',',':'))+'\n');(dest/'summary.json').write_text(json.dumps(summary,indent=2)+'\n');print(json.dumps({'out':str(dest),**summary},indent=2))
if __name__=='__main__':main()
