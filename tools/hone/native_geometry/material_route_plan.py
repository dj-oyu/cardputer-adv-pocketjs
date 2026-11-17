"""Bind authored polygons and shader drivers to graphs; not a shader evaluator."""
import ast,collections,json,pathlib,re,sys


def plan(package,inventory,routes):
    if (package.get('schema')!='hone.source-preservation.v1' or inventory.get('schema')!='hone.material-inventory.v1' or
        routes.get('schema')!='hone.driver-route-plan.v1' or any(x.get('runtime_ready') is not False for x in (package,inventory,routes)) or
        not package.get('source_sha256') or any(x.get('source_sha256')!=package['source_sha256'] for x in (inventory,routes))):
        raise ValueError('source identity')
    meshes=package['meshes'];mesh_ids={m['name']:i for i,m in enumerate(meshes)}
    materials=inventory['materials'];by_name={m['name']:m for m in materials}
    if len(mesh_ids)!=len(meshes) or len(by_name)!=len(materials):raise ValueError('duplicate identity')
    uses={};node_types=collections.Counter();unsupported=collections.Counter()
    for mat in materials:
        if mat.get('library') is not None or mat.get('use_nodes') is not True:raise ValueError('unsupported material origin')
        nodes={n['name']:n for n in mat['nodes']}
        if len(nodes)!=len(mat['nodes']):raise ValueError('duplicate node')
        if sum(n['type']=='ShaderNodeOutputMaterial' and n.get('active_output') is True for n in nodes.values())!=1:
            raise ValueError('active material output')
        for n in nodes.values():
            node_types[n['type']]+=1
            if n.get('mute') is not False:unsupported['muted_or_unknown_node']+=1
            if n.get('group_tree') is not None:unsupported['group_tree']+=1
            if n.get('image') is not None:unsupported['image']+=1
        linked=set()
        for link in mat['links']:
            src=nodes.get(link['source_node']);dst=nodes.get(link['target_node']);si=link['source_socket'];di=link['target_socket']
            if src is None or dst is None or type(si) is not int or type(di) is not int or not 0<=si<len(src['outputs']) or not 0<=di<len(dst['inputs']):
                raise ValueError('invalid graph link')
            if link.get('muted') is not False:unsupported['muted_or_unknown_link']+=1
            k=(dst['name'],di)
            if k in linked and not dst['inputs'][di].get('is_multi_input',False):raise ValueError('ambiguous input links')
            linked.add(k)
        for use in mat['uses']:
            mid=mesh_ids.get(use['object']);slot=use['slot']
            if mid is None or type(slot) is not int or not 0<=slot<len(meshes[mid]['material_slots']):raise ValueError('material use')
            if meshes[mid]['material_slots'][slot]!=mat['name'] or (mid,slot) in uses:raise ValueError('material slot mismatch')
            expected=[i for i,p in enumerate(meshes[mid]['polygons']) if p['material']==slot]
            if use['polygons']!=expected:raise ValueError('polygon material mismatch')
            uses[(mid,slot)]=mat['name']
    for mid,m in enumerate(meshes):
        for face in m['polygons']:
            slot=face['material']
            if type(slot) is not int or (mid,slot) not in uses:raise ValueError('unbound polygon')
    shader=[]
    for route in routes['routes']:
        if route['kind']!='shader_property':continue
        owners=route['owner'].get('embedded_in')
        if not isinstance(owners,list) or len(owners)!=1 or owners[0].get('type')!='Material' or owners[0].get('name') not in by_name:
            raise ValueError('unbound shader owner')
        mat=by_name[owners[0]['name']]
        match=re.fullmatch(r'nodes\[("(?:[^"\\]|\\.)*")\]\.(inputs|outputs)\[(\d+)\]\.default_value',route['data_path'])
        if not match:raise ValueError('unsupported shader driver path')
        try:node_name=ast.literal_eval(match.group(1))
        except (ValueError,SyntaxError) as error:raise ValueError('shader node name') from error
        nodes={n['name']:n for n in mat['nodes']};node=nodes.get(node_name);direction=match.group(2);index=int(match.group(3))
        if node is None or index>=len(node[direction]):raise ValueError('shader socket')
        value=node[direction][index]['default']
        if value.get('status')!='ok':raise ValueError('shader default unavailable')
        component=route['array_index'];default=value.get('value')
        if type(component) is not int or component<0 or (isinstance(default,list) and component>=len(default)) or (not isinstance(default,list) and component!=0):
            raise ValueError('shader component')
        shader.append(dict(program=route['program'],material=mat['name'],node=node_name,direction=direction,socket=index,component=component))
    return dict(schema='hone.material-route-plan.v1',runtime_ready=False,source_sha256=package['source_sha256'],
                materials=len(materials),mesh_slot_uses=len(uses),polygons=sum(len(m['polygons']) for m in meshes),
                shader_routes=shader,node_types=dict(node_types),unsupported=dict(unsupported),
                pending=['evaluate shader graph and driver values in same frame','coordinate and geometry inputs',
                         'transparent composition, colors and target material mapping','image parity and runtime memory'])


if __name__=='__main__':
    if len(sys.argv)!=5:raise SystemExit('expected source, material inventory, driver routes, new output')
    a,b,c=(json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in sys.argv[1:4]);result=plan(a,b,c)
    with pathlib.Path(sys.argv[4]).open('x',encoding='utf8',newline='\n') as out:json.dump(result,out,separators=(',',':'),allow_nan=False)
    print(json.dumps(dict(runtime_ready=False,materials=result['materials'],polygons=result['polygons'],
                          mesh_slot_uses=result['mesh_slot_uses'],shader_routes=len(result['shader_routes']),unsupported=result['unsupported'])))
