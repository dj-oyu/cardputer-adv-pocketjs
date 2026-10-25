"""Exercise the exact finite device JS and injected native marker on the host.
Timing/heap from this check are not device measurements.
"""
import hashlib,json,os,subprocess
import common,prepare_device
R=common.R

def main():
 out=R/'.cache/lcd7-device';apps=common.inputs(out);hsrc=(R/'tools/games/test_lcd_catch_host.c').read_text()
 hsrc=common.once(hsrc,'int main(void){', '''extern bool lcd_diag_finished(void),lcd_diag_valid(void);
extern void lcd_diag_finish(const char *);
int main(int argc,char **argv){''')
 hsrc=common.once(hsrc,'FILE *f=fopen("apps/lcdcatch/lcd_catch.js","rb");','FILE *f=fopen(argv[1],"rb");')
 hsrc=common.once(hsrc,'    printf("session 1: GAME A, pause, game over, GAME B, Back\\n");','''    if(argc>1){open_session();while(!lcd_diag_finished()&&frames_run<210)frame();CHECK(lcd_diag_valid(),"finite exact game composition");lcd_diag_finish(lcd_diag_valid()?"complete":"input_mismatch");FILE *p=fopen(getenv("LCD7_PANEL"),"wb");fwrite(panel,1,sizeof panel,p);fclose(p);close_session();return fails||exceptions||bad_present||!lcd_diag_valid();}
    printf("session 1: GAME A, pause, game over, GAME B, Back\\n");''')
 h=out/'device.c';h.write_text(hsrc)
 native=out/'pocket_input.c';native.write_text(prepare_device.input_source((R/'main/pocket/pocket_input.c').read_text()))
 build=out/'build';build.mkdir(parents=True,exist_ok=True)
 (build/'esp_heap_caps.h').write_text('#pragma once\n#include <stddef.h>\n#define MALLOC_CAP_INTERNAL 0\n#define MALLOC_CAP_8BIT 0\nstatic inline size_t heap_caps_get_free_size(unsigned flags){(void)flags;return 0;}\n')
 common.game.EXTRA=[str(native) if p=='main/pocket/pocket_input.c' else p for p in common.game.EXTRA]
 binary=common.build(build,h);logs={};panels={}
 for mode,app in apps.items():
  app.write_text(app.read_text()+'\n'+(prepare_device.TEMPLATE/'workload.js').read_text());dest=out/(mode+'.panel')
  log=subprocess.check_output([str(binary),str(app)],cwd=R,env={**{k:v for k,v in os.environ.items() if not k.startswith(('LCD_','LCD7_'))},'LCD7_PANEL':str(dest)},text=True)
  assert 'frames=120' in log and 'play=84 miss=36' in log and 'complete:true' in log,log
  (out/(mode+'.log')).write_text(log);logs[mode]=log;panels[mode]=dest.read_bytes()
 assert logs['baseline']==logs['candidate'];assert panels['baseline']==panels['candidate']
 report=dict(workload=prepare_device.WORKLOAD,frames=120,setup_frames=73,panel_sha256=hashlib.sha256(panels['baseline']).hexdigest(),logs_equal=True,host_only=True)
 (out/'result.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
