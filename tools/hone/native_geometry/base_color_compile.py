"""Compile an explicitly restricted Principled base-color graph, not lighting."""
import json,math,pathlib,struct,sys
from alpha_factor_compile import emit as admit_alpha, OPS


def emit(inventory,material_routes):
    # Reuse the strict active surface/opacity/coordinate admission gate.
    admit_alpha(inventory,material_routes)
    result=['/* Generated base-color candidates; not renderer admission. */\n',
            '#ifndef HONE_PRIVATE_BASE_COLORS_H\n#define HONE_PRIVATE_BASE_COLORS_H\n#include "base_color_candidate.h"\n']
    programs=[];used=set()
    def literal(v):
        if type(v) not in (int,float) or not math.isfinite(v):raise ValueError('scalar default')
        try:x=struct.unpack('<f',struct.pack('<f',v))[0]
        except (OverflowError,struct.error) as error:raise ValueError('float default') from error
        if not math.isfinite(x):raise ValueError('nonfinite float')
        return x.hex()+'f'
    for mid,material in enumerate(inventory['materials']):
        nodes={n['name']:n for n in material['nodes']};links={}
        for link in material['links']:
            if link.get('muted') is not False:raise ValueError('muted link')
            key=(link['target_node'],link['target_socket'])
            if key in links:raise ValueError('ambiguous link')
            links[key]=(link['source_node'],link['source_socket'])
        driven={}
        for r in material_routes['shader_routes']:
            if r['material']==material['name']:
                key=(r['node'],r['direction'],r['socket'],r['component'])
                if key in driven:raise ValueError('duplicate driver')
                driven[key]=r['program']
        principals=[n for n in nodes.values() if n['type']=='ShaderNodeBsdfPrincipled']
        if len(principals)!=1:raise ValueError('ambiguous principled shader')
        principal=principals[0];base=[s for s in principal['inputs'] if s['name']=='Base Color']
        if len(base)!=1:raise ValueError('base color socket')
        for channel in range(3):
            code=[];memo={};visiting=set()
            def add(op,a=0,b=0,v='0x0p+0f'):
                if len(code)>=1024:raise ValueError('color program limit')
                code.append((op,a,b,v));return len(code)-1
            def const(v):return add('HONE_ALPHA_CONST',v=literal(v))
            def value(node,direction,index,component):
                route=(node['name'],direction,index,component)
                if route in driven:
                    program=driven[route]
                    if type(program) is not int or not 0<=program<=65535:raise ValueError('driver index')
                    used.add((material['name'],route));return add('HONE_ALPHA_DRIVER',a=program)
                sockets=node[direction]
                if not 0<=index<len(sockets):raise ValueError('missing socket')
                default=sockets[index]['default']
                if default.get('status')!='ok':raise ValueError('socket default')
                v=default['value']
                if isinstance(v,list):
                    if component>=len(v):raise ValueError('socket component')
                    v=v[component]
                elif component!=0:raise ValueError('scalar component')
                return const(v)
            def scalar_input(node,index):
                key=(node['name'],index)
                return scalar(*links[key]) if key in links else value(node,'inputs',index,0)
            def color_input(node,index,component):
                key=(node['name'],index)
                return color(*links[key],component) if key in links else value(node,'inputs',index,component)
            def scalar(name,index):
                key=('scalar',name,index)
                if key in memo:return memo[key]
                if key in visiting or name not in nodes:raise ValueError('cyclic or missing scalar node')
                visiting.add(key);node=nodes[name];kind=node['type']
                if node.get('mute') is not False:raise ValueError('muted node')
                if kind=='ShaderNodeValue' and index==0:r=value(node,'outputs',0,0)
                elif kind=='ShaderNodeSeparateXYZ' and 0<=index<=2:
                    source=links.get((name,0));coord=nodes.get(source[0]) if source else None
                    if (coord is None or coord['type']!='ShaderNodeTexCoord' or coord.get('mute') is not False or
                        coord.get('from_instancer') is not False or 'object' not in coord or coord['object'] is not None or
                        type(source[1]) is not int or not 0<=source[1]<len(coord['outputs']) or coord['outputs'][source[1]]['name']!='Object'):
                        raise ValueError('unsupported color coordinates')
                    r=add(('HONE_ALPHA_X','HONE_ALPHA_Y','HONE_ALPHA_Z')[index])
                elif kind=='ShaderNodeMath' and index==0:
                    op=node.get('operation')
                    if op not in OPS or type(node.get('use_clamp')) is not bool:raise ValueError('unsupported math')
                    a=scalar_input(node,0);b=0 if op=='SQRT' else scalar_input(node,1);r=add(OPS[op],a,b)
                    if node['use_clamp']:r=add('HONE_ALPHA_CLAMP',r)
                else:raise ValueError('unsupported color scalar')
                visiting.remove(key);memo[key]=r;return r
            def color(name,index,component):
                key=('color',name,index,component)
                if key in memo:return memo[key]
                if key in visiting or name not in nodes:raise ValueError('cyclic or missing color node')
                visiting.add(key);node=nodes[name];kind=node['type']
                if node.get('mute') is not False:raise ValueError('muted node')
                if kind in ('ShaderNodeValue','ShaderNodeMath') and index==0:r=scalar(name,index)
                elif kind=='ShaderNodeMixRGB' and index==0:
                    if node.get('blend_type')!='MIX' or node.get('use_alpha') is not False or type(node.get('use_clamp')) is not bool:raise ValueError('unsupported color blend')
                    factor=add('HONE_ALPHA_CLAMP',scalar_input(node,0));a=color_input(node,1,component);b=color_input(node,2,component)
                    inv=add('HONE_ALPHA_SUB',const(1),factor);r=add('HONE_ALPHA_ADD',add('HONE_ALPHA_MUL',a,inv),add('HONE_ALPHA_MUL',b,factor))
                    if node['use_clamp']:r=add('HONE_ALPHA_CLAMP',r)
                elif kind=='ShaderNodeValToRGB' and index==0:
                    ramp=node.get('color_ramp',{});elements=ramp.get('elements')
                    if ramp.get('interpolation')!='LINEAR' or ramp.get('color_mode')!='RGB' or not isinstance(elements,list) or len(elements)<2:
                        raise ValueError('unsupported color ramp')
                    x=scalar_input(node,0);positions=[];colors=[]
                    for e in elements:
                        pos=e.get('position');rgba=e.get('color')
                        if type(pos) not in (int,float) or not math.isfinite(pos) or not isinstance(rgba,list) or len(rgba)!=4 or any(type(v) not in (int,float) or not math.isfinite(v) or not 0<=v<=1 for v in rgba):raise ValueError('ramp element')
                        positions.append(pos);colors.append(rgba[component])
                    if any(positions[i]>=positions[i+1] for i in range(len(positions)-1)):raise ValueError('ramp positions')
                    r=const(colors[0])
                    for i in range(len(positions)-1):
                        numerator=add('HONE_ALPHA_SUB',x,const(positions[i]));ratio=add('HONE_ALPHA_DIV',numerator,const(positions[i+1]-positions[i]));weight=add('HONE_ALPHA_CLAMP',ratio)
                        r=add('HONE_ALPHA_ADD',r,add('HONE_ALPHA_MUL',weight,const(colors[i+1]-colors[i])))
                else:raise ValueError('unsupported color graph node')
                visiting.remove(key);memo[key]=r;return r
            last=color_input(principal,base[0]['index'],channel)
            name='hone_base_code_%d_%d'%(mid,channel)
            result.append('static const hone_alpha_instruction %s[] = {%s};\n'%(name,','.join('{%s,%d,%d,%s}'%x for x in code)))
            programs.append('{%s,%d,%d}'%(name,len(code),last))
    result.append('static const hone_alpha_program hone_base_color_programs[] = {%s};\n'%(','.join(programs) if programs else '{0,0,0}'))
    result.append('static const size_t hone_base_color_material_count = %d;\n#endif\n'%len(inventory['materials']))
    return ''.join(result),dict(runtime_ready=False,materials=len(inventory['materials']),color_driver_routes=len(used),other_shader_routes=len(material_routes['shader_routes'])-len(used))


if __name__=='__main__':
    if len(sys.argv)!=4:raise SystemExit('expected material inventory, material routes, new header')
    inv,routes=(json.loads(pathlib.Path(x).read_text(encoding='utf8')) for x in sys.argv[1:3]);header,summary=emit(inv,routes)
    with pathlib.Path(sys.argv[3]).open('x',encoding='utf8',newline='\n') as out:out.write(header)
    print(json.dumps(summary))
