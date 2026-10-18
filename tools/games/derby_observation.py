"""Strict analysis of bounded diagnostic samples and controlled draw fixtures.

These are observations, not binary attestation, allocator peaks, or LCD proof.
"""
import math

INTEGER_FIELDS = 'v run us begin frame ctx race phase cam tier step u gu_sample_max cap eval_used eval_cap refused seq limit io_us_max'.split()

def parse_line(line):
    tail = line[line.index('DERBY_TLM ') + len('DERBY_TLM '):]
    pairs = [part.split('=', 1) for part in tail.strip().split()]
    if any(len(p) != 2 for p in pairs) or len({p[0] for p in pairs}) != len(pairs):
        raise ValueError('invalid_or_duplicate_fields')
    raw = dict(pairs)
    if set(raw) != set(INTEGER_FIELDS + ['seed', 'intervals']):
        raise ValueError('unknown_or_missing_fields')
    row = {k:int(raw[k]) for k in INTEGER_FIELDS}
    row['seed'] = int(raw['seed'], 16)
    row['intervals'] = [tuple(map(int, p.split(':'))) for p in raw['intervals'].split(',')]
    if (row['v'] != 1 or len(row['intervals']) != 10 or not 0 <= row['cam'] < 10 or
        not 0 <= row['phase'] <= 4 or not 0 <= row['tier'] <= 2 or
        not 0 <= row['seed'] <= 0xffffffff or row['limit'] != 300 or
        not 1 <= row['seq'] <= row['limit']):
        raise ValueError('unsupported_schema')
    if any(row[k] < 0 for k in INTEGER_FIELDS if k != 'u') or not -1 <= row['u'] <= 1000000:
        raise ValueError('invalid_counter')
    if any(len(p)!=2 or min(p)<0 for p in row['intervals']):
        raise ValueError('invalid_intervals')
    return row

def camera_windows(rows):
    result=[]
    for a,b in zip(rows,rows[1:]):
        # Same endpoint camera is insufficient if another camera intervened:
        # require every frame and every context in the interval accounted for.
        delta=b['frame']-a['frame']
        cam=b['cam']; na,ta=a['intervals'][cam]; nb,tb=b['intervals'][cam]
        stable=all(a[k]==b[k] for k in ('run','begin','seed','race','phase','cam','tier'))
        monotone=all(bn>=an and bt>=at for (an,at),(bn,bt) in zip(a['intervals'],b['intervals']))
        other_cameras_unchanged=all(a['intervals'][i]==b['intervals'][i] for i in range(10) if i!=cam)
        valid=(stable and b['seq']==a['seq']+1 and b['us']-a['us']>=1000000 and delta>0
               and b['ctx']-a['ctx']==delta and nb-na==delta and tb>ta
               and tb-ta==b['us']-a['us'] and monotone and other_cameras_unchanged
               and a['refused']==b['refused']==0 and b['seq']<b['limit'])
        if valid:
            result.append({'camera':cam,'frames':delta,'device_interval_us':tb-ta,
                           'diagnostic_fps':delta*1e6/(tb-ta),
                           'io_us_max_prior_writes':b['io_us_max']})
    return result

def compare_fixture(expected, observed, tolerance=1e-7):
    if observed.get('kind')!='controlled_draw_inputs' or observed.get('camera_cut') is not True:
        raise ValueError('controlled_inputs_required_not_screenshot')
    for key in ('seed','oval','step','camera','interpolation','tier'):
        if observed.get(key)!=expected[key]:
            raise ValueError('fixture_identity_mismatch:'+key)
    def equal(a,b):
        if isinstance(a,(float,int)) and not isinstance(a,bool):
            if not isinstance(b,(float,int)) or isinstance(b,bool) or not math.isfinite(b) or abs(a-b)>tolerance:
                raise ValueError('numeric_mismatch')
        elif isinstance(a,list):
            if not isinstance(b,list) or len(a)!=len(b):raise ValueError('shape_mismatch')
            for x,y in zip(a,b):equal(x,y)
        elif isinstance(a,dict):
            if not isinstance(b,dict) or set(a)!=set(b):raise ValueError('shape_mismatch')
            for k in a:equal(a[k],b[k])
        elif a!=b:raise ValueError('value_mismatch')
    for key in ('xs','depths','projected','drawn','side'):
        if key not in observed:raise ValueError('missing_rendering_inputs:'+key)
        equal(expected[key],observed[key])
    return True

def compare_artifacts(reference, observation):
    for key in ('source_commit','source_tree','camera_cut_policy'):
        if not reference.get(key) or reference[key]!=observation.get(key):
            raise ValueError('provenance_or_camera_policy_mismatch:'+key)
    keys=('seed','oval','step','camera','interpolation','tier')
    index={tuple(row[k] for k in keys):row for row in reference['frames']}
    seen=set()
    if not observation.get('frames'):raise ValueError('no_observations')
    for row in observation['frames']:
        identity=tuple(row[k] for k in keys)
        if identity in seen:raise ValueError('duplicate_fixture')
        seen.add(identity)
        if identity not in index:raise ValueError('unmatched_fixture')
        compare_fixture(index[identity],row)
    return len(seen)

if __name__=='__main__':
    import argparse,json
    from pathlib import Path
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference',type=Path)
    parser.add_argument('--observed',type=Path)
    parser.add_argument('--telemetry',type=Path)
    args=parser.parse_args()
    if args.telemetry:
        rows=[parse_line(line) for line in args.telemetry.read_text().splitlines() if 'DERBY_TLM ' in line]
        windows=camera_windows(rows)
        print(json.dumps(dict(samples=len(rows),windows=windows,
            sampled_heap_lower_bound=max((r['gu_sample_max'] for r in rows),default=None),
            scope='diagnostic throughput and sampled memory; not allocator peak or LCD proof'),indent=2))
    elif args.reference and args.observed:
        print(json.dumps({'matched_controlled_draw_inputs':compare_artifacts(
            json.loads(args.reference.read_text()),json.loads(args.observed.read_text()))}))
    else:parser.error('supply --telemetry or --reference and --observed')
