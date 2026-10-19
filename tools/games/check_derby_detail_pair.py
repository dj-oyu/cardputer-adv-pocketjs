"""Verify actual ON/OFF fixture identity and retain a functional overhead control."""
import argparse,json,statistics
from pathlib import Path
from derby_fixed_result import parse_trial
from derby_detail_result import parse_detail
p=argparse.ArgumentParser();p.add_argument('--off',type=Path,required=True);p.add_argument('--on',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
results=[]
for directory,enabled in ((a.off,False),(a.on,True)):
 r=json.loads((directory/'result.json').read_text());t=parse_trial((directory/'trial.log').read_bytes(),transport_complete=True);detail=parse_detail((directory/'trial.log').read_text(),expected=enabled)
 if r['detail_enabled']!=enabled or r['detail']!=detail or r['trial']!=t:raise ValueError('saved result/stream mismatch')
 results.append(r)
off,on=results
if off['variant']!=on['variant'] or off['windows']!=on['windows'] or off['transfers']!=on['transfers']:raise ValueError('ON/OFF work or pixel mismatch')
for x,y in zip(off['trial']['repetitions'],on['trial']['repetitions'],strict=True):
 for k in ('contexts','done','finish',*(f'c{i}' for i in range(10))):
  if x[k]!=y[k]:raise ValueError('fixed semantic work differs')
 for i in range(10):
  if x[f's{i}'].split(':')[1]!=y[f's{i}'].split(':')[1]:raise ValueError('coarse work call count differs')
report={'pass':True,'variant':on['variant'],'hardware_tested':False,'scope':'host functional overhead control; dependent replay and oracle work; not target timing','off_work_us':[int(r['work_us']) for r in off['trial']['repetitions']],'on_work_us':[int(r['work_us']) for r in on['trial']['repetitions']]}
report['median_ratio']=statistics.median(report['on_work_us'])/statistics.median(report['off_work_us']);a.out.write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report))
