"""Compile a narrowly admitted local-coordinate opacity factor; not a shader."""
import json,math,pathlib,struct,sys

OPS={'ADD':'HONE_ALPHA_ADD','SUBTRACT':'HONE_ALPHA_SUB','MULTIPLY':'HONE_ALPHA_MUL',
     'DIVIDE':'HONE_ALPHA_DIV','MINIMUM':'HONE_ALPHA_MIN','MAXIMUM':'HONE_ALPHA_MAX',
     'LESS_THAN':'HONE_ALPHA_LT','GREATER_THAN':'HONE_ALPHA_GT','SQRT':'HONE_ALPHA_SQRT'}


def emit(inventory,material_routes):
    if (inventory.get('schema')!='hone.material-inventory.v1' or
        material_routes.get('schema')!='hone.material-route-plan.v1' or
        inventory.get('runtime_ready') is not False or material_routes.get('runtime_ready') is not False or
        not inventory.get('source_sha256') or inventory['source_sha256']!=material_routes.get('source_sha256') or
        material_routes.get('unsupported')):raise ValueError('material plan identity or unsupported graph')
    material_names={m['name'] for m in inventory['materials']}
    if (len(material_names)!=len(inventory['materials']) or material_routes.get('materials')!=len(material_names) or
        any(r.get('material') not in material_names for r in material_routes['shader_routes'])):
        raise ValueError('material route coverage')
    output=['/* Generated alpha candidates, not shader or renderer admission. */\n',
            '#ifndef HONE_PRIVATE_ALPHA_FACTORS_H\n#define HONE_PRIVATE_ALPHA_FACTORS_H\n#include "alpha_factor_candidate.h"\n']
    programs=[];used=set();mix_count=0
    def literal(v):
        if type(v) not in (int,float) or not math.isfinite(v):raise ValueError('scalar default')
        try:value=struct.unpack('<f',struct.pack('<f',v))[0]
        except (OverflowError,struct.error) as error:raise ValueError('float default') from error
        if not math.isfinite(value):raise ValueError('nonfinite default')
        return value.hex()+'f'
    for mid,material in enumerate(inventory['materials']):
        nodes={n['name']:n for n in material['nodes']}
        if len(nodes)!=len(material['nodes']):raise ValueError('duplicate node')
        links={}
        for link in material['links']:
            if link.get('muted') is not False:raise ValueError('muted link')
            key=(link['target_node'],link['target_socket'])
            if key in links:raise ValueError('ambiguous link')
            links[key]=(link['source_node'],link['source_socket'])
        driven={}
        for route in material_routes['shader_routes']:
            if route['material']==material['name']:
                key=(route['node'],route['direction'],route['socket'],route['component'])
                if key in driven:raise ValueError('duplicate shader route')
                driven[key]=route['program']
        code=[];memo={};visiting=set()
        def add(op,a=0,b=0,v='0x0p+0f'):
            if len(code)>=1024:raise ValueError('alpha program limit')
            code.append((op,a,b,v));return len(code)-1
        def default(node,index):
            if index>=len(node['inputs']):raise ValueError('missing input')
            route=(node['name'],'inputs',index,0)
            if route in driven:
                program=driven[route]
                if type(program) is not int or not 0<=program<=65535:raise ValueError('driver index')
                used.add((material['name'],route));return add('HONE_ALPHA_DRIVER',a=program)
            data=node['inputs'][index]['default']
            if data.get('status')!='ok':raise ValueError('missing scalar default')
            return add('HONE_ALPHA_CONST',v=literal(data['value']))
        def input_value(node,index):
            target=(node['name'],index)
            return expression(*links[target]) if target in links else default(node,index)
        def expression(name,index):
            key=(name,index)
            if key in memo:return memo[key]
            if key in visiting or name not in nodes:raise ValueError('cyclic or missing node')
            visiting.add(key);node=nodes[name];kind=node['type']
            if node.get('mute') is not False:raise ValueError('muted node')
            if kind=='ShaderNodeValue' and index==0:
                route=(name,'outputs',0,0)
                if route in driven:
                    program=driven[route]
                    if type(program) is not int or not 0<=program<=65535:raise ValueError('driver index')
                    used.add((material['name'],route));result=add('HONE_ALPHA_DRIVER',a=program)
                else:
                    data=node['outputs'][0]['default']
                    if data.get('status')!='ok':raise ValueError('value output')
                    result=add('HONE_ALPHA_CONST',v=literal(data['value']))
            elif kind=='ShaderNodeSeparateXYZ' and 0<=index<=2:
                source=links.get((name,0));coord=nodes.get(source[0]) if source else None
                if (coord is None or coord['type']!='ShaderNodeTexCoord' or coord.get('mute') is not False or coord.get('from_instancer') is not False or 'object' not in coord or coord['object'] is not None or
                    type(source[1]) is not int or not 0<=source[1]<len(coord['outputs']) or coord['outputs'][source[1]]['name']!='Object'):
                    raise ValueError('only object-local coordinates admitted')
                result=add(('HONE_ALPHA_X','HONE_ALPHA_Y','HONE_ALPHA_Z')[index])
            elif kind=='ShaderNodeMath' and index==0:
                op=node.get('operation')
                if op not in OPS or type(node.get('use_clamp')) is not bool:raise ValueError('unsupported math')
                a=input_value(node,0);b=0 if op=='SQRT' else input_value(node,1)
                result=add(OPS[op],a,b)
                if node['use_clamp']:result=add('HONE_ALPHA_CLAMP',result)
            else:raise ValueError('unsupported alpha graph node')
            visiting.remove(key);memo[key]=result;return result
        outputs=[n for n in nodes.values() if n['type']=='ShaderNodeOutputMaterial' and n.get('active_output') is True]
        if len(outputs)!=1:raise ValueError('material output')
        output_node=outputs[0]
        if output_node.get('mute') is not False:raise ValueError('muted output')
        if any((output_node['name'],i) in links for i in (1,2)):raise ValueError('volume/displacement unsupported')
        surface=links.get((output_node['name'],0));shader=nodes.get(surface[0]) if surface else None
        if shader is None or surface[1]!=0:raise ValueError('unlinked or unsupported surface output')
        if shader.get('mute') is not False:raise ValueError('muted shader')
        if shader['type']=='ShaderNodeMixShader':
            sources=[links.get((shader['name'],i)) for i in (1,2)]
            if any(s is None or s[0] not in nodes or s[1]!=0 for s in sources):raise ValueError('unlinked mix')
            transparent,principal=(nodes[s[0]] for s in sources)
            if (transparent['type']!='ShaderNodeBsdfTransparent' or principal['type']!='ShaderNodeBsdfPrincipled' or
                transparent.get('mute') is not False or principal.get('mute') is not False):raise ValueError('unsupported shader mix')
            color=transparent['inputs'][0]['default']
            if ((transparent['name'],0) in links or any(k[0]==transparent['name'] and k[1:3]==('inputs',0) for k in driven) or
                color.get('status')!='ok' or color.get('value')!=[1.,1.,1.,1.]):raise ValueError('colored transparency')
            factor=input_value(shader,0);result=add('HONE_ALPHA_CLAMP',factor);mix_count+=1
        elif shader['type']=='ShaderNodeBsdfPrincipled':principal=shader;result=add('HONE_ALPHA_CONST',v='0x1p+0f')
        else:raise ValueError('unsupported surface')
        alpha=[s for s in principal['inputs'] if s['name']=='Alpha']
        if (len(alpha)!=1 or (principal['name'],alpha[0]['index']) in links or
            any(k[0]==principal['name'] and k[1:3]==('inputs',alpha[0]['index']) for k in driven) or
            alpha[0]['default'].get('status')!='ok' or alpha[0]['default'].get('value')!=1.):raise ValueError('principled alpha')
        name='hone_alpha_code_%d'%mid
        output.append('static const hone_alpha_instruction %s[] = {%s};\n'%(name,','.join('{%s,%d,%d,%s}'%x for x in code)))
        programs.append('{%s,%d,%d}'%(name,len(code),result))
    output.append('static const hone_alpha_program hone_alpha_programs[] = {%s};\n'%(','.join(programs) if programs else '{0,0,0}'))
    output.append('static const size_t hone_alpha_program_count = %d;\n#endif\n'%len(programs))
    return ''.join(output),dict(runtime_ready=False,materials=len(programs),transparent_mix_materials=mix_count,
                                alpha_driver_routes=len(used),other_shader_routes=len(material_routes['shader_routes'])-len(used))


if __name__=='__main__':
    if len(sys.argv)!=4:raise SystemExit('expected material inventory, material routes, new header')
    a,b=(json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in sys.argv[1:3]);header,summary=emit(a,b)
    with pathlib.Path(sys.argv[3]).open('x',encoding='utf8',newline='\n') as out:out.write(header)
    print(json.dumps(summary))
