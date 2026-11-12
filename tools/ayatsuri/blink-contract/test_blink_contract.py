import json,hashlib,pathlib,struct
from blink_fixture import evaluate,mask

values=[0,.5,.979999,.98,.980001,1]
cases=[(b,l,r) for b in values for l in values for r in values]
rows=[]
for both,left,right in cases:
    parts=evaluate(both,left,right)
    # Independent selection, segment interpolation and inequality oracle.
    for idx,local in enumerate((left,right)):
        b=both if both>=local else local;p=parts[idx];side=(-1,1)[idx]
        expected_upper=(1-b)*1+b*0
        assert p['upper'][0][1]==expected_upper and p['lower'][1][1]==-expected_upper
        for x in (3*side-1.1,3*side-1,3*side,3*side+1,3*side+1.1):
            for y in (-1,-.5,-.020001,-.02,0,.02,.020001,.5,1):
                expected=(b<.98 and 3*side-1<=x<=3*side+1 and -1+b<=y<=1-b)
                assert mask(p,x,y)==expected
        assert p['visible']==(b<.98)
    rows.append({'controls':[both,left,right],'parts':parts})
# Reopen/reverse seek/pause are stateless reproducibility, not native snapshot proof.
sequence=[(0,0,0),(.5,0,0),(1,0,0),(.5,0,0),(0,0,0)]
assert evaluate(*sequence[0])==evaluate(*sequence[-1])
assert evaluate(*sequence[1])==evaluate(*sequence[3])
pending=evaluate(.5,0,0);saved=json.dumps(pending,sort_keys=True)
invalid=[(float('nan'),0,0),(float('inf'),0,0),(-.1,0,0),(1.1,0,0),(0,None,0),(0,True,0),(0,0,'1')]
for args in invalid:
    try:evaluate(*args)
    except ValueError:pass
    else:raise AssertionError('invalid accepted')
    assert json.dumps(pending,sort_keys=True)==saved
# A hide-only opaque rectangle disagrees with the half-blink spatial oracle.
p=evaluate(.5,0,0)[0];assert p['visible'] and not mask(p,-3,.75)
# Quantization before threshold can flip this input: retain full specified controls.
b=.979999;assert b<.98 and round(b*255)/255>=.98
b=.97999999; f32=struct.unpack('<f',struct.pack('<f',b))[0]
assert b<.98 and f32>=.98
report={'schema':'public-synthetic-blink-v1','controlCases':len(cases),'maskChecks':len(cases)*2*5*9,'invalidCases':len(invalid),'runtimeVerified':False,'privateMaterialEquivalent':False,'cases':rows}
pathlib.Path('report.json').write_text(json.dumps(report,indent=2))
print('PASS',report['controlCases'],report['maskChecks'],report['invalidCases'])
