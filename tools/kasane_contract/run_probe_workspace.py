"""Compare all existing probe scenes; HAL/timing are synthetic, not hardware."""
from pathlib import Path
import argparse,hashlib,json,os,subprocess
R=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--baseline',required=True,type=Path);p.add_argument('--sanitize',action='store_true');a=p.parse_args()
out=R/'.cache'/('probe-workspace-sanitized' if a.sanitize else 'probe-workspace');inc=out/'include';inc.mkdir(parents=True,exist_ok=True)
stubs={
 'sdkconfig.h':'#pragma once\n#define CONFIG_KSN_DEVICE_PROBE 1\n',
 'esp_heap_caps.h':'#pragma once\n#include <stddef.h>\n#define MALLOC_CAP_8BIT 1u\nsize_t heap_caps_get_free_size(unsigned);\nsize_t heap_caps_get_largest_free_block(unsigned);\n',
 'esp_log.h':'#pragma once\nvoid test_probe_log(const char*,const char*,const char*,...);\n#define ESP_LOGI(t,...) test_probe_log("I",t,__VA_ARGS__)\n#define ESP_LOGE(t,...) test_probe_log("E",t,__VA_ARGS__)\n#define ESP_LOGW(t,...) test_probe_log("W",t,__VA_ARGS__)\n',
 'freertos/FreeRTOS.h':'#pragma once\ntypedef unsigned TickType_t;\n#define pdMS_TO_TICKS(x) ((unsigned)(x))\n',
 'freertos/task.h':'#pragma once\nvoid vTaskDelay(unsigned);\nunsigned uxTaskGetStackHighWaterMark(void*);\n',
}
for name,source in stubs.items():
 q=inc/name;q.parent.mkdir(parents=True,exist_ok=True);q.write_text(source)
subprocess.run(['python3','tools/make_font.py',str(inc)],cwd=R,check=True)
common=['ksn_view.c','ksn_core.c','ksn_cache.c','ksn_modal.c','ksn_render.c','ksn_blend_pie.c','ksn_video_frames.c','ksn_depth_lines.c','ksn_frost.c','ksn_stress_probe.c']
flags=['-std=gnu11','-O2','-g','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers']
if a.sanitize:flags+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
commands=[];reports=[]
for label,source in [('baseline',a.baseline/'main/ui/kasane/ksn_device_probe.c'),('shared',R/'main/ui/kasane/ksn_device_probe.c')]:
 binary=out/label
 cmd=['cc',*flags,'-DPROBE_SOURCE="'+str(source)+'"',*(['-DTEST_SHARED_WORKSPACE=1'] if label=='shared' else []),'-I'+str(inc),'-Itools/kasane_contract/fontshim','-Itools/hostshim','-Imain','-Imain/hal','-Imain/text','-Imain/scene','-Imain/ui/kasane','-Itools/kasane_contract','tools/kasane_contract/test_probe_workspace.c',*[str(R/'main/ui/kasane'/name) for name in common],'main/text/ksn_font.c','-lm','-o',str(binary)]
 commands.append(cmd);subprocess.run(cmd,cwd=R,check=True)
 result=subprocess.run([str(binary)],cwd=R,check=True,text=True,capture_output=True,env=dict(os.environ,ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1',PROBE_PIXEL_TRACE=str(out/(label+'.pixels')),PROBE_MESSAGE_TRACE=str(out/(label+'.messages'))))
 (out/(label+'.log')).write_text(result.stdout+result.stderr);print(result.stdout,end='')
 reports.append([l for l in result.stdout.splitlines() if l.startswith('HOST_PROBE ')])
assert reports[0]==reports[1],reports
parity={}
for suffix in ['pixels','messages']:
 left=out/('baseline.'+suffix);right=out/('shared.'+suffix)
 h=hashlib.sha256();count=0
 with left.open('rb') as a_file,right.open('rb') as b_file:
  while True:
   a_bytes,b_bytes=a_file.read(65536),b_file.read(65536)
   assert a_bytes==b_bytes,(suffix,count)
   if not a_bytes:break
   h.update(a_bytes);count+=len(a_bytes)
 parity[suffix]={'bytes':count,'sha256':h.hexdigest(),'byte_equal':True}
print('BYTE_PARITY '+json.dumps(parity,sort_keys=True))
(out/'parity.json').write_text(json.dumps(parity,indent=2)+'\n')

(out/'commands.json').write_text(json.dumps(commands,indent=2)+'\n')
print('All diagnostic scenes and repeated runs: baseline/shared pixel streams equal; virtual times are not performance measurements')
