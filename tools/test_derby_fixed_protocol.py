"""Public v1 schemas, sample and bounded collector acceptance contract."""
from pathlib import Path
from copy import deepcopy
import hashlib
import json
import sys
from jsonschema import Draft202012Validator

ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tools/games'))
from derby_fixed_result import parse_collected,parse_trial,TRANSPORT_FLAGS

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
                   lambda x:x['repetitions'][1].update(rep='3'),
                   lambda x:x['repetitions'][0].update(p0='1:2'),
                   lambda x:x['repetitions'][0].update(variant='unknown'),
                   lambda x:x['repetitions'][0].update(extra='1')]:
        bad=deepcopy(parsed);modify(bad);assert list(validators[0].iter_errors(bad))
    print('PASS public v1 schemas/sample and bounded collector: all loss, reset, truncation, limit and digest cases rejected')

if __name__=='__main__':main()
