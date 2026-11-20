"""Create private C driver programs and binding metadata; no runtime admission."""
import hashlib, json, pathlib, sys
from driver_bytecode import compile_expression


def emit(inventory):
    if inventory.get('schema')!='hone.driver-inventory.v1' or inventory.get('runtime_ready') is not False:
        raise ValueError('unsupported inventory')
    records=inventory['drivers']
    if len(records)>1024:raise ValueError('driver count')
    header=['/* Private generated candidate; no frame or material admission. */\n',
            '#ifndef HONE_PRIVATE_DRIVER_PROGRAMS_H\n#define HONE_PRIVATE_DRIVER_PROGRAMS_H\n',
            '#include "driver_bytecode_candidate.h"\n']
    bindings=[];inputs=[];input_index={};slots=[]
    for index,driver in enumerate(records):
        if driver['type']!='SCRIPTED' or driver['mute'] or driver.get('curve_valid') is not True or driver.get('driver_valid') is not True or driver.get('keyframe_count')!=0:
            raise ValueError('unsupported driver state')
        modifiers=driver.get('fcurve_modifiers')
        if not isinstance(modifiers,list):raise ValueError('missing modifier inventory')
        for mod in modifiers:
            if mod.get('mute') is True:continue
            if (mod.get('type')!='GENERATOR' or mod.get('mode')!='POLYNOMIAL' or
                mod.get('poly_order')!=1 or mod.get('coefficients')!=[0.0,1.0] or
                mod.get('use_additive') is not False or mod.get('use_restricted_range') is not False or
                mod.get('use_influence') is not False):raise ValueError('unsupported FCurve modifier')
        variables=driver['variables'];names=[v['name'] for v in variables]
        for var in variables:
            if var['type']!='SINGLE_PROP' or len(var['targets'])!=1 or var['targets'][0]['current']['status']!='ok':
                raise ValueError('unsupported binding')
            target=var['targets'][0]
            if not isinstance(target.get('id'),dict) or not isinstance(target.get('data_path'),str) or not target['data_path']:
                raise ValueError('missing input identity')
        program=compile_expression(driver['expression'],names)
        rows=['{'+str(op)+','+str(arg)+','+float(value).hex()+'}' for op,arg,value in program]
        header.append('static const hone_driver_instruction hone_driver_%d[] = {%s};\n'%(index,','.join(rows)))
        first=len(slots)
        for var in variables:
            target=var['targets'][0];identity=dict(id=target['id'],data_path=target['data_path'])
            key=json.dumps(identity,sort_keys=True,separators=(',',':'),ensure_ascii=False)
            if key not in input_index:
                if len(inputs)>=65536:raise ValueError('input count')
                input_index[key]=len(inputs);inputs.append(identity)
            slots.append(input_index[key])
        if len(slots)>65535:raise ValueError('slot count')
        bindings.append(dict(program=index,first=first,owner=driver['owner'],data_path=driver['data_path'],
            array_index=driver['array_index'],variables=variables))
    rows=['{hone_driver_%d,sizeof hone_driver_%d/sizeof hone_driver_%d[0],%d,%d}'%(i,i,i,len(b['variables']),b['first']) for i,b in enumerate(bindings)]
    header.append('static const hone_driver_program hone_driver_programs[] = {%s};\n'%(','.join(rows) if rows else '{0,0,0,0}'))
    header.append('static const uint16_t hone_driver_input_slots[] = {%s};\n'%(','.join(map(str,slots)) if slots else '0'))
    header.append('static const size_t hone_driver_program_count = %d, hone_driver_slot_count = %d, hone_driver_input_count = %d;\n#endif\n'%(len(rows),len(slots),len(inputs)))
    return ''.join(header),dict(schema='hone.driver-bindings.v1',runtime_ready=False,
        source_sha256=inventory['source_sha256'],inputs=inputs,programs=bindings)


def build(source,directory):
    raw=pathlib.Path(source).read_bytes();inventory=json.loads(raw);header,bindings=emit(inventory)
    dest=pathlib.Path(directory);dest.mkdir(parents=True,exist_ok=False)
    data=header.encode('utf8');mapping=json.dumps(bindings,ensure_ascii=False,separators=(',',':'),allow_nan=False).encode('utf8')
    (dest/'driver_programs.private.h').write_bytes(data);(dest/'driver_bindings.private.json').write_bytes(mapping)
    return dict(runtime_ready=False,programs=len(bindings['programs']),inventory_sha256=hashlib.sha256(raw).hexdigest(),
        header_sha256=hashlib.sha256(data).hexdigest(),bindings_sha256=hashlib.sha256(mapping).hexdigest())


if __name__=='__main__':
    if len(sys.argv)!=3:raise SystemExit('expected inventory and new directory')
    print(json.dumps(build(*sys.argv[1:])))
