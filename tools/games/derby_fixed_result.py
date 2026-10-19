"""Strict whole-trial parser; invalid reps never disappear from the decision."""
import hashlib
import json
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from derby_fixed_workload_chunks import FIXTURE,MANIFEST_SHA256

TRANSPORT_FLAGS=('gaps','overflow','truncated','partial_tail','disconnect','reset','refused')
# c4 counts only the instrumented clip/cache-normalization sqrt sites.
# It is not a count of all square roots in the app or renderer.
EXPECTED_WORK={
    'baseline':(0,120,600,7735,7735,0,0,0,0,0),
    'native':(0,120,600,7735,120,7735,0,120,0,0),
    'batch':(0,120,600,7735,7735,0,0,0,120,960),
}
EXPECTED_BRIDGE_CALLS={'baseline':2086,'native':2086,'batch':1246}
# Guest calls follow continuation dispatches. VM/points/raster/transfer/observer
# calls can depend on continuation slicing and host/device instrumentation.
# Only the bridge/native/procedural-build work calls must repeat unchanged.
FIXED_CALL_STAGES=(1,2,3)

def verify_work(records,variant):
    """Shared host/device comparison gate after format and time validation."""
    if variant not in EXPECTED_WORK or len(records)!=3:
        raise ValueError('work_variant_or_repetition_count')
    first_calls=None
    for row in records:
        if tuple(int(row[f'c{i}']) for i in range(10))!=EXPECTED_WORK[variant]:
            raise ValueError('known_work_counters_differ')
        calls=[int(row[f's{i}'].split(':')[1]) for i in range(10)]
        if calls[1]!=EXPECTED_BRIDGE_CALLS[variant]:
            raise ValueError('known_bridge_calls_differ')
        if not calls[2] or not calls[3]:
            raise ValueError('native_or_build_work_missing')
        fixed=tuple(calls[i] for i in FIXED_CALL_STAGES)
        if first_calls is not None and fixed!=first_calls:
            raise ValueError('fixed_work_calls_differ_between_repetitions')
        first_calls=fixed
        if calls[0]!=120+int(row['cont']):
            raise ValueError('guest_calls_do_not_match_dispatches')
        if calls[4]<calls[3] or calls[6]<120 or calls[7]<120:
            raise ValueError('renderer_work_missing')
        if calls[8]:
            raise ValueError('output_inside_measured_work')

def parse_collected(data:bytes,transport:dict):
    """Validate a bounded collector receipt, then the complete device trial.

    The owner must establish these transport facts; matching JSON alone does
    not prove that the underlying acquisition was lossless.
    """
    if set(transport)!={'complete','bytes','sha256','duration_ms',*TRANSPORT_FLAGS}:
        raise ValueError('transport_schema')
    if transport['complete'] is not True or any(transport[k] is not False for k in TRANSPORT_FLAGS):
        raise ValueError('transport_quality')
    if type(transport['bytes']) is not int or not 0<transport['bytes']<=65536 or transport['bytes']!=len(data):
        raise ValueError('transport_byte_limit_or_length')
    if type(transport['duration_ms']) is not int or not 0<=transport['duration_ms']<=200000:
        raise ValueError('transport_deadline')
    if transport['sha256']!=hashlib.sha256(data).hexdigest():
        raise ValueError('transport_digest')
    if any(len(line)>1024 for line in data.splitlines(keepends=True) if line.startswith(b'DERBY_FIXED')):
        raise ValueError('diagnostic_line_limit')
    return parse_trial(data,transport_complete=True)

def parse_trial_format(data:bytes,*,transport_complete:bool):
    """Format/identity/time checks only; never qualifies a performance trial.

    Synthetic controller tests use this entry point. Device collection and
    comparison must use parse_trial/parse_collected instead.
    """
    if not transport_complete or not data.endswith(b'\n'):
        raise ValueError('partial_or_incomplete_transport')
    text=data.decode('ascii')
    records=[];end=[]
    fixed_keys=set('v fixture manifest variant session rep seq valid quality independent seed oval tier cam start end contexts done cont capture faults refused tx_fail wall_us work_us finish'.split())|{f'p{i}' for i in range(3)}|{f'c{i}' for i in range(10)}|{f's{i}' for i in range(10)}
    for line in text.splitlines():
        if not line.startswith('DERBY_FIXED'):continue
        parts=line.split();pairs=[x.split('=',1) for x in parts[1:]]
        if any(len(x)!=2 for x in pairs) or len({x[0] for x in pairs})!=len(pairs):
            raise ValueError('malformed_or_duplicate_field')
        row=dict(pairs)
        if parts[0]=='DERBY_FIXED_END':
            if set(row)!=set('v session reps valid quality independent'.split()):raise ValueError('end_schema')
            end.append(row)
        elif parts[0]=='DERBY_FIXED':
            if end or set(row)!=fixed_keys:raise ValueError('summary_schema_or_order')
            records.append(row)
        else:raise ValueError('unknown_fixed_record')
    if len(records)!=3 or len(end)!=1:raise ValueError('three_repetitions_and_final_record_required')
    identity={'v':'1','fixture':FIXTURE['id'],'manifest':MANIFEST_SHA256,'valid':'1','quality':'0',
              'independent':'0','seed':FIXTURE['seed'],'oval':'1','tier':'1','cam':'8','start':'390','end':'510',
              'contexts':'120','done':'120','capture':'0','faults':'0','refused':'0','tx_fail':'0','finish':FIXTURE['finish']}
    session=records[0]['session'];variant=records[0]['variant']
    if variant not in ('baseline','native','batch'):raise ValueError('unknown_variant')
    for i,row in enumerate(records,1):
        if any(row[k]!=v for k,v in identity.items()) or row['rep']!=str(i) or row['seq']!=str(i):
            raise ValueError('invalid_repetition_identity_quality_or_sequence')
        if row['session']!=session or row['variant']!=variant:raise ValueError('mixed_session_or_variant')
        for key in ('session','cont','wall_us','work_us'):
            if not row[key].isdigit() or int(row[key])>2**64-1:raise ValueError('invalid_counter')
        if not 0<int(row['work_us'])<=int(row['wall_us'])<=180000000:raise ValueError('invalid_wall_or_work')
        for prefix,count,width in [('p',3,3),('c',10,1),('s',10,2)]:
            for j in range(count):
                values=row[f'{prefix}{j}'].split(':')
                if len(values)!=width or any(not v.isdigit() or int(v)>2**64-1 for v in values):raise ValueError('invalid_tuple')
        pace=sum(int(row[f'p{j}'].split(':')[0]) for j in range(3))
        if pace+int(row['work_us'])>int(row['wall_us']):raise ValueError('direct_intervals_exceed_wall')
        stages=[tuple(map(int,row[f's{j}'].split(':'))) for j in range(10)]
        if any(self_us>int(row['wall_us']) or (not calls and self_us) for self_us,calls in stages):
            raise ValueError('impossible_stage_self')
        if sum(self_us for self_us,_ in stages)>int(row['work_us']):
            raise ValueError('stage_self_sum_exceeds_direct_work')
    if end[0]!={'v':'1','session':session,'reps':'3','valid':'1','quality':'0','independent':'0'}:
        raise ValueError('invalid_whole_trial')
    return {'fixture':FIXTURE,'manifest_sha256':MANIFEST_SHA256,'variant':variant,
            'session':session,'independent_trials':False,'format_valid':True,
            'work_verified':False,'all_repetitions_valid':False,
            'repetitions':records,'transport_sha256':hashlib.sha256(data).hexdigest()}

def parse_trial(data:bytes,*,transport_complete:bool):
    trial=parse_trial_format(data,transport_complete=transport_complete)
    verify_work(trial['repetitions'],trial['variant'])
    trial.update(work_verified=True,all_repetitions_valid=True)
    return trial

if __name__=='__main__':
    import argparse
    from pathlib import Path
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path)
    quality=p.add_mutually_exclusive_group(required=True)
    quality.add_argument('--complete-transport',action='store_true',help='caller certifies complete transport')
    quality.add_argument('--transport',type=Path,help='bounded collector receipt JSON')
    args=p.parse_args();data=args.log.read_bytes()
    result=parse_collected(data,json.loads(args.transport.read_text())) if args.transport else parse_trial(data,transport_complete=True)
    print(json.dumps(result,indent=2))
