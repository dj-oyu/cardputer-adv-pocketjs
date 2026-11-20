"""Identify constant base-color inputs and retain procedural cases explicitly."""
import json,math,pathlib,sys
from provider_binding import binding_plan


def plan(package,inventory,material_routes,scene):
    if (package.get('schema')!='hone.source-preservation.v1' or inventory.get('schema')!='hone.material-inventory.v1' or
        material_routes.get('schema')!='hone.material-route-plan.v1' or scene.get('schema')!='hone.scene-node-plan.v1' or
        any(x.get('runtime_ready') is not False for x in (package,inventory,material_routes,scene)) or
        not package.get('source_sha256') or any(x.get('source_sha256')!=package['source_sha256'] for x in (inventory,material_routes,scene))):
        raise ValueError('source identity')
    if material_routes.get('unsupported'):raise ValueError('unsupported material structure')
    records=[];names={}
    for material in inventory['materials']:
        if material['name'] in names:raise ValueError('duplicate material')
        nodes=[n for n in material['nodes'] if n['type']=='ShaderNodeBsdfPrincipled']
        if len(nodes)!=1:raise ValueError('ambiguous principled material')
        node=nodes[0];sockets=[s for s in node['inputs'] if s['name']=='Base Color']
        if len(sockets)!=1:raise ValueError('base color socket')
        socket=sockets[0];index=socket['index']
        linked=any(l['target_node']==node['name'] and l['target_socket']==index and l['muted'] is False for l in material['links'])
        driven=any(r['material']==material['name'] and r['node']==node['name'] and r['direction']=='inputs' and r['socket']==index for r in material_routes['shader_routes'])
        row=dict(material=material['name'],kind='graph' if linked else 'driver' if driven else 'constant')
        if row['kind']=='constant':
            value=socket['default']
            if value.get('status')!='ok' or not isinstance(value.get('value'),list) or len(value['value'])!=4 or any(type(x) not in (int,float) or not math.isfinite(x) or not 0<=x<=1 for x in value['value']):
                raise ValueError('invalid constant color')
            row['linear_rgba']=value['value']
        names[material['name']]=len(records);records.append(row)
    parts=binding_plan(package)['nodes'];mapping=[];seen=set()
    for node in scene['nodes']:
        if node['kind']!='gouraudMesh':continue
        ordinal=node['part']
        if type(ordinal) is not int or not 0<=ordinal<len(parts) or ordinal in seen:raise ValueError('part assignment')
        seen.add(ordinal);part=parts[ordinal]
        if node['source_mesh']!=part['source_mesh']:raise ValueError('mesh mismatch')
        mesh=package['meshes'][part['source_mesh']];name=mesh['material_slots'][part['material_slot']]
        if name not in names:raise ValueError('unbound material')
        mapping.append(dict(node=node['index'],material=names[name],kind=records[names[name]]['kind']))
    if len(seen)!=len(parts):raise ValueError('missing geometry part')
    return dict(schema='hone.base-color-plan.v1',runtime_ready=False,source_sha256=package['source_sha256'],
                materials=records,nodes=mapping,pending=['procedural base-color graphs','principled lighting/specular mapping',
                'per-fragment alpha and compositing','scene color and display calibration'])


if __name__=='__main__':
    if len(sys.argv)!=6:raise SystemExit('expected source, materials, material routes, scene plan, new output')
    data=[json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in sys.argv[1:5]];result=plan(*data)
    with pathlib.Path(sys.argv[5]).open('x',encoding='utf8',newline='\n') as out:json.dump(result,out,separators=(',',':'),allow_nan=False)
    from collections import Counter
    print(json.dumps(dict(runtime_ready=False,material_kinds=dict(Counter(x['kind'] for x in result['materials'])),
                          node_kinds=dict(Counter(x['kind'] for x in result['nodes'])))))
