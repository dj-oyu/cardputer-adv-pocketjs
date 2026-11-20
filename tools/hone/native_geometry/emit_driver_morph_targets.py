"""Emit admitted relative/unmasked morph routes; no runtime admission."""
import ast
import json
import math
import pathlib
import re
import struct
import sys


def emit(package,route_plan):
    if (package.get('schema')!='hone.source-preservation.v1' or
        route_plan.get('schema')!='hone.driver-route-plan.v1' or
        package.get('runtime_ready') is not False or route_plan.get('runtime_ready') is not False or
        not package.get('source_sha256') or package.get('source_sha256')!=route_plan.get('source_sha256')):
        raise ValueError('source identity')
    offsets=[0];defaults=[];blocks=[]
    for mesh in package['meshes']:
        keys=mesh['shape_keys']
        if keys is None:blocks.append([]);offsets.append(len(defaults));continue
        if keys.get('use_relative') is not True:raise ValueError('absolute morph not admitted')
        names=[]
        for key in keys['blocks']:
            if (not isinstance(key.get('name'),str) or key['name'] in names or
                key.get('mute') is not False or key.get('vertex_group') != ''):
                raise ValueError('unsupported morph block')
            value=key.get('value')
            if type(value) not in (int,float) or not math.isfinite(value):raise ValueError('morph default')
            try:value=struct.unpack('<f',struct.pack('<f',value))[0]
            except (OverflowError,struct.error) as error:raise ValueError('morph default overflow') from error
            names.append(key['name']);defaults.append(value)
        blocks.append(names);offsets.append(len(defaults))
    rows=[];seen=set();remaining={}
    for route in route_plan['routes']:
        if route['kind']!='shape_key':
            remaining[route['kind']]=remaining.get(route['kind'],0)+1;continue
        match=re.fullmatch(r'key_blocks\[("(?:[^"\\]|\\.)*")\]\.value',route['data_path'])
        if not match or route.get('array_index')!=0:raise ValueError('unsupported shape path')
        try:name=ast.literal_eval(match.group(1))
        except (SyntaxError,ValueError) as error:raise ValueError('shape key string') from error
        program=route['program']
        if type(program) is not int or not 0<=program<=65535:raise ValueError('program index')
        meshes=route.get('source_meshes')
        if not isinstance(meshes,list) or not meshes:raise ValueError('missing shape targets')
        for mid in meshes:
            if type(mid) is not int or not 0<=mid<len(blocks) or name not in blocks[mid]:raise ValueError('shape target missing')
            target=offsets[mid]+blocks[mid].index(name)
            if target in seen:raise ValueError('duplicate shape target')
            seen.add(target);rows.append((program,target))
    if len(defaults)>1000000 or len(rows)>4096:raise ValueError('morph route limit')
    floats=','.join(float(x).hex()+'f' for x in defaults) or '0'
    routes=','.join('{%d,%d}'%x for x in rows) or '{0,0}'
    header=('/* Generated candidate, unmasked relative shape keys only. */\n'
            '#ifndef HONE_PRIVATE_MORPH_TARGETS_H\n#define HONE_PRIVATE_MORPH_TARGETS_H\n'
            '#include "driver_morph_targets_candidate.h"\n'
            'static const float hone_morph_defaults[] = {%s};\n'
            'static const uint32_t hone_morph_offsets[] = {%s};\n'
            'static const hone_driver_morph_route hone_morph_routes[] = {%s};\n'
            'static const size_t hone_morph_count = %d, hone_morph_route_count = %d;\n#endif\n')%(floats,','.join(map(str,offsets)),routes,len(defaults),len(rows))
    return header,dict(runtime_ready=False,morph_values=len(defaults),routes=len(rows),unbound_programs=sum(remaining.values()),unbound_kinds=remaining)


if __name__=='__main__':
    if len(sys.argv)!=4:raise SystemExit('expected source package, route plan and new header')
    package,routes=(json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in sys.argv[1:3])
    header,summary=emit(package,routes)
    with pathlib.Path(sys.argv[3]).open('x',encoding='utf8',newline='\n') as out:out.write(header)
    print(json.dumps(summary))
