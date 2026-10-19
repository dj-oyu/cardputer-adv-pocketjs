"""Preserve compiler stack-use evidence; not a complete runtime stack proof."""
import argparse
import json
from pathlib import Path

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--build',type=Path,required=True)
    args=parser.parse_args();build=args.build.resolve()
    files=sorted(build.rglob('*derby_fixed_workload*.su'))
    if not files:raise SystemExit('fixed workload compiler stack-use evidence missing')
    rows=[]
    for path in files:
        for line in path.read_text().splitlines():
            fields=line.split('\t')
            if len(fields)!=3 or not fields[1].isdigit():raise ValueError('unexpected compiler stack format')
            rows.append(dict(function=fields[0].rsplit(':',1)[-1],bytes=int(fields[1]),kind=fields[2],
                             artifact=path.relative_to(build).as_posix()))
    report=dict(scope='compiler per-function stack-use only; no whole JS/native call-chain or runtime high-water proof',
                max_function_bytes=max(row['bytes'] for row in rows),functions=rows)
    (build/'fixed-stack.json').write_text(json.dumps(report,indent=2)+'\n')
    print('DERBY_FIXED_STACK '+json.dumps(report,sort_keys=True))

if __name__=='__main__':main()
