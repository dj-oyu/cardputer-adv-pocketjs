"""Exact native marker/workload vs ordinary real-input bot; no device timing claims."""
from pathlib import Path
import argparse,json,os,subprocess,hashlib
import common,prepare_device
R=common.R

def main():
 p=argparse.ArgumentParser();p.add_argument('--m32',action='store_true');a=p.parse_args();out=R/('.cache/bigwave-rock-device-m32' if a.m32 else '.cache/bigwave-rock-device');apps=common.inputs(out);s=(R/'tools/games/test_big_wave_host.c').read_text()
 s=common.once(s,'static void present(void){','static FILE *panels;\nstatic void present(void){')
 s=common.once(s,'    else pocket_kasane_animations_presented(now);','    else pocket_kasane_animations_presented(now);\n    if(panels)fwrite(panel,1,sizeof panel,panels);')
 s=common.once(s,'int main(int argc,char **argv){','extern bool rock_diag_finished(void),rock_diag_valid(void);\nextern void rock_diag_finish(const char *);\nint main(int argc,char **argv){\n    panels=fopen(getenv("BW_PANELS"),"wb");')
 s=common.once(s,'    /* ---- 1. title:', '''    if(getenv("ROCK_DIAG")){
        while(!rock_diag_finished()&&frame_no<220)frames(1,"");
        expect(rock_diag_valid(),"finite diagnostic120ride","frames=%u",frame_no);
        rock_diag_finish(rock_diag_valid()?"complete":"input_mismatch");
    }else{
        frames(40,"");frames(1,"1");frames(1,"");for(unsigned i=0;i<160;i++)frames(1,bot(0));
    }
    fclose(panels);printf("ROCK_DEVICE frames=%u state=%.0f x=%.9f exceptions=%u present=%u oracle=%u\\n",frame_no,S_("st"),S_("x"),exceptions,present_failures,oracle_failures);
    pocket_input_reset();pocket_kasane_reset();JS_FreeContext(ctx);JS_FreeRuntime(rt);
    return fails||exceptions||present_failures||oracle_failures;
    /* ---- 1. title:''')
 h=out/'test.c';h.write_text(s);native=out/'pocket_input.c';n=prepare_device.input_source((R/'main/pocket/pocket_input.c').read_text());n=n.replace('memset(&now,0,sizeof now); /* diagnostic empty physical input */','if(getenv("ROCK_DIAG"))memset(&now,0,sizeof now); /* host control uses real FIFO */');native.write_text("#include <stdlib.h>\n"+n)
 build=out/'build';build.mkdir(exist_ok=True);(build/'esp_heap_caps.h').write_text('#pragma once\n#include <stddef.h>\n#define MALLOC_CAP_INTERNAL 0\n#define MALLOC_CAP_8BIT 0\nstatic inline size_t heap_caps_get_free_size(unsigned n){(void)n;return 0;}\n')
 common.game.EXTRA=[str(native) if p=='main/pocket/pocket_input.c' else p for p in common.game.EXTRA];binary=common.build(build,h,m32=a.m32)
 report={};control=None
 for label,mode,diag in [('control','baseline',False),('baseline','baseline',True),('candidate','candidate',True)]:
  app=out/(label+'-input.js');app.write_text(apps[mode].read_text()+ ('\n'+(prepare_device.TEMPLATE/'workload.js').read_text() if diag else ''))
  dest=out/(label+'.panels');env={**{k:v for k,v in os.environ.items() if not k.startswith(('BW_','BIGWAVE_','ROCK_'))},'BW_HEAP_LIMIT':'163840' if a.m32 else '524288','BW_PANELS':str(dest)}
  if diag:env['ROCK_DIAG']='1'
  log=subprocess.check_output([str(binary),str(app)],cwd=R,env=env,text=True);(out/(label+'.log')).write_text(log)
  if diag:assert 'complete:true' in log and 'ride=120 other=0' in log
  data=dest.read_bytes()
  if control is None:control=data
  else:assert control==data,label
  report[label]=dict(panels=len(data)//64800,sha256=hashlib.sha256(data).hexdigest(),state=next(l for l in log.splitlines() if l.startswith('ROCK_DEVICE ')))
 assert report['control']['state']==report['baseline']['state']==report['candidate']['state']
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
