"""Target object/symbol and compiler stack evidence, not runtime high-water proof."""
import argparse,json,subprocess,re
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True);a=p.parse_args();build=a.build.resolve()
text=subprocess.check_output(['xtensa-esp32s3-elf-nm','-S','--defined-only',str(build/'cardputer_pocketjs.elf')],text=True)
symbols=[]
for line in text.splitlines():
 m=re.fullmatch(r'([0-9a-fA-F]+) ([0-9a-fA-F]+) ([A-Za-z]) (dd_\w+)',line.strip())
 if m:symbols.append({'name':m[4],'bytes':int(m[2],16),'section_type':m[3]})
mode='ON' if any(s['name']=='dd_start' for s in symbols) else 'OFF'
stack=[]
for f in sorted(build.rglob('*.su')):
 if not any(n in f.name for n in ('derby_measure','derby_fixed_workload','pocket_derby','pocket_proc','ksn_procedural','ksn_proc_plan','ksn_render','board.c','app_session')):continue
 for line in f.read_text().splitlines():
  fields=line.split('\t')
  if len(fields)==3 and fields[1].isdigit():stack.append({'function':fields[0].rsplit(':',1)[-1],'bytes':int(fields[1]),'kind':fields[2],'artifact':f.relative_to(build).as_posix()})
if not stack:raise ValueError('compiler stack artifacts missing')
if mode=='ON' and not {'dd_totals','dd_stack','dd_saved'}<={s['name'] for s in symbols}:raise ValueError('detail state missing from actual ELF')
report={'detail_mode':mode,'symbols':symbols,'detail_state_bytes':sum(s['bytes'] for s in symbols if s['section_type'].lower() in ('b','d')),'compiler_stack':stack,'max_function_stack_bytes':max(s['bytes'] for s in stack),'scope':'actual linked symbols and per-function compiler stack; frame snapshot also grows fixed-controller state; no whole call-chain or runtime heap/stack acceptance'}
(build/'detail-resources.json').write_text(json.dumps(report,indent=2)+'\n');print('DERBY_DETAIL_RESOURCES '+json.dumps(report,sort_keys=True))
