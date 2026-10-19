"""Public v1 schemas, sample and bounded collector acceptance contract."""
from pathlib import Path
from copy import deepcopy
import hashlib
import json
import sys
from jsonschema import Draft202012Validator

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/games'))
from derby_fixed_result import parse_collected,parse_trial,parse_trial_format,TRANSPORT_FLAGS,EXPECTED_WORK,EXPECTED_BRIDGE_CALLS

def main():
    base=ROOT/'tools/games';sample=base/'fixtures/derby_fixed_v1'
    raw=sample.with_suffix('.log').read_bytes()
    receipt=json.loads(Path(str(sample)+'.transport.json').read_text())
    parsed=json.loads(Path(str(sample)+'.parsed.json').read_text())
    validators=[]
    for file,value in [('derby_fixed_v1.schema.json',parsed),('derby_fixed_transport.schema.json',receipt)]:
        schema=json.loads((base/file).read_text());Draft202012Validator.check_schema(schema)
        validator=Draft202012Validator(schema);validator.validate(value);validators.append(validator)
    assert parse_collected(raw,receipt)==parsed
    assert parse_trial(raw,transport_complete=True)==parsed
    assert parsed['work_verified'] and parsed['all_repetitions_valid']
    def encode(rows):
        lines=['DERBY_FIXED '+' '.join(f'{k}={v}' for k,v in row.items()) for row in rows]
        lines.append(f"DERBY_FIXED_END v=1 session={rows[0]['session']} reps=3 valid=1 quality=0 independent=0")
        return ('\n'.join(lines)+'\n').encode('ascii')
    def rejected(rows,expected):
        data=encode(rows)
        quality=dict(receipt,bytes=len(data),sha256=hashlib.sha256(data).hexdigest())
        for parse in (lambda:parse_trial(data,transport_complete=True),lambda:parse_collected(data,quality)):
            try:parse()
            except ValueError as error:assert str(error)==expected,(str(error),expected)
            else:raise AssertionError('invalid work/time qualified for comparison')
    originals=parsed['repetitions']
    for variant,counts in EXPECTED_WORK.items():
        rows=deepcopy(originals)
        for row in rows:
            row['variant']=variant
            for i,value in enumerate(counts):row[f'c{i}']=str(value)
            row['s1']=row['s1'].split(':')[0]+':'+str(EXPECTED_BRIDGE_CALLS[variant])
        assert parse_trial(encode(rows),transport_complete=True)['work_verified']
        for i in range(10):
            bad=deepcopy(rows)
            for row in bad:row[f'c{i}']=str(counts[i]+1)
            rejected(bad,'known_work_counters_differ')
        bad=deepcopy(rows)
        for row in bad:
            for i in range(10):row[f'c{i}']='0'
        form=parse_trial_format(encode(bad),transport_complete=True)
        assert form['format_valid'] and not form['work_verified'] and not form['all_repetitions_valid']
        assert list(validators[0].iter_errors(form))
        rejected(bad,'known_work_counters_differ')
        bad=deepcopy(rows)
        for row in bad:
            for i in range(10):row[f's{i}']='0:0'
        rejected(bad,'known_bridge_calls_differ')
        bad=deepcopy(rows)
        for row in bad:row['s1']='0:'+str(EXPECTED_BRIDGE_CALLS[variant]+1)
        rejected(bad,'known_bridge_calls_differ')
        for stage in (2,3):
            bad=deepcopy(rows)
            for row in bad:row[f's{stage}']='0:0'
            rejected(bad,'native_or_build_work_missing')
            bad=deepcopy(rows);self_us,calls=bad[1][f's{stage}'].split(':')
            bad[1][f's{stage}']=self_us+':'+str(int(calls)+1)
            rejected(bad,'fixed_work_calls_differ_between_repetitions')
        # Continuation and renderer/observer work can vary without adding
        # another context. Do not freeze those calls to the host's profile.
        changed=deepcopy(rows);changed[1]['cont']='3'
        self_us,_=changed[1]['s0'].split(':');changed[1]['s0']=self_us+':123'
        for stage in (4,5,6,7,9):
            self_us,calls=changed[1][f's{stage}'].split(':')
            changed[1][f's{stage}']=self_us+':'+str(int(calls)+2)
        assert parse_trial(encode(changed),transport_complete=True)['work_verified']
        bad=deepcopy(changed);bad[1]['s0']=bad[1]['s0'].split(':')[0]+':120'
        rejected(bad,'guest_calls_do_not_match_dispatches')
        for stage in (4,6,7):
            bad=deepcopy(rows);bad[1][f's{stage}']='0:0'
            rejected(bad,'renderer_work_missing')
        bad=deepcopy(rows);bad[1]['s8']='0:1'
        rejected(bad,'output_inside_measured_work')
    bad=deepcopy(originals);bad[0]['s2']=str(int(bad[0]['wall_us'])+1)+':4569'
    rejected(bad,'impossible_stage_self')
    bad=deepcopy(originals);bad[0]['s5']='1:0'
    rejected(bad,'impossible_stage_self')
    bad=deepcopy(originals)
    for i in (0,1,2,3,4,6,7):bad[0][f's{i}']=str(int(bad[0]['work_us'])//2)+':'+bad[0][f's{i}'].split(':')[1]
    rejected(bad,'stage_self_sum_exceeds_direct_work')
    invalid=[]
    for key in TRANSPORT_FLAGS:
        item=dict(receipt);item[key]=True;invalid.append(item)
    for key,value in [('bytes',len(raw)-1),('bytes',True),('duration_ms',200001),
                      ('duration_ms',True),('complete',False),('sha256','0'*64)]:
        item=dict(receipt);item[key]=value;invalid.append(item)
    item=dict(receipt);item['extra']=False;invalid.append(item)
    item=dict(receipt);del item['reset'];invalid.append(item)
    for item in invalid:
        try:parse_collected(raw,item)
        except ValueError:pass
        else:raise AssertionError('invalid collector receipt accepted')
    long_raw=raw+b'DERBY_FIXED '+b'x'*1024+b'\n'
    item=dict(receipt,bytes=len(long_raw),sha256=hashlib.sha256(long_raw).hexdigest())
    try:parse_collected(long_raw,item)
    except ValueError as error:assert str(error)=='diagnostic_line_limit'
    else:raise AssertionError('oversized diagnostic line accepted')
    for modify in [lambda x:x.update(independent_trials=True),
                   lambda x:x.update(work_verified=False),
                   lambda x:x['repetitions'][1].update(rep='3'),
                   lambda x:x['repetitions'][0].update(p0='1:2'),
                   lambda x:x['repetitions'][0].update(variant='unknown'),
                   lambda x:x['repetitions'][0].update(extra='1')]:
        bad=deepcopy(parsed);modify(bad);assert list(validators[0].iter_errors(bad))
    print('PASS public v1 schemas/sample, transport, common work gate: zero/wrong/repetition calls and impossible self rejected; continuation variability accepted')

if __name__=='__main__':main()
