"""Record a private Blender material-graph inventory, never runtime admission.

This retains nodes, sockets, links and selected shader/math/ramp parameters.
It deliberately does not claim complete Blender node semantics or rendering.
"""
import bpy, hashlib, json, math, pathlib, sys


def encode_default(socket):
    if not hasattr(socket,'default_value'): return {'status':'absent'}
    value=socket.default_value
    if type(value) in (bool,int,str): return {'status':'ok','value':value}
    if isinstance(value,float):
        return {'status':'ok','value':value} if math.isfinite(value) else {'status':'non_finite'}
    try:
        data=list(value)
        if all(type(x) in (bool,int,float) and (not isinstance(x,float) or math.isfinite(x)) for x in data):
            return {'status':'ok','value':data}
    except TypeError: pass
    return {'status':'unsupported'}


def sockets(items):
    return [dict(index=i,identifier=s.identifier,name=s.name,type=s.bl_idname,
                 is_multi_input=bool(getattr(s,'is_multi_input',False)),default=encode_default(s)) for i,s in enumerate(items)]


def node_record(node):
    result=dict(name=node.name,type=node.bl_idname,inputs=sockets(node.inputs),outputs=sockets(node.outputs),
                mute=bool(node.mute),active_output=getattr(node,'is_active_output',None))
    for key in ('operation','blend_type','use_clamp','use_alpha','data_type','interpolation_type','from_instancer'):
        if hasattr(node,key):
            value=getattr(node,key)
            if type(value) in (str,bool,int,float):result[key]=value
    if hasattr(node,'color_ramp'):
        ramp=node.color_ramp
        result['color_ramp']=dict(interpolation=ramp.interpolation,color_mode=ramp.color_mode,
            elements=[dict(position=e.position,color=list(e.color)) for e in ramp.elements])
    if hasattr(node,'object'):
        obj=node.object;result['object']=None if obj is None else dict(type=obj.bl_rna.identifier,name=obj.name_full)
    if hasattr(node,'node_tree'):
        tree=node.node_tree;result['group_tree']=None if tree is None else dict(name=tree.name_full,
            library=tree.library.filepath if tree.library else None)
    if hasattr(node,'image'):
        image=node.image;result['image']=None if image is None else dict(name=image.name_full,
            library=image.library.filepath if image.library else None,source=image.source,filepath=image.filepath)
    result['internal_links']=[dict(input_index=next(i for i,s in enumerate(node.inputs) if s==link.from_socket),
                                  output_index=next(i for i,s in enumerate(node.outputs) if s==link.to_socket))
                              for link in node.internal_links]
    return result


def inventory():
    materials={slot.material for obj in bpy.context.scene.objects if obj.type=='MESH'
               for slot in obj.material_slots if slot.material is not None}
    records=[]
    for material in sorted(materials,key=lambda x:x.name_full):
        tree=material.node_tree if material.use_nodes else None
        nodes=[] if tree is None else [node_record(n) for n in tree.nodes]
        links=[] if tree is None else [dict(source_node=link.from_node.name,
            source_socket=next(i for i,s in enumerate(link.from_node.outputs) if s==link.from_socket),
            target_node=link.to_node.name,
            target_socket=next(i for i,s in enumerate(link.to_node.inputs) if s==link.to_socket),
            muted=link.is_muted,multi_input_sort_id=getattr(link,'multi_input_sort_id',None)) for link in tree.links]
        uses=[]
        for obj in bpy.context.scene.objects:
            if obj.type!='MESH':continue
            for slot_index,slot in enumerate(obj.material_slots):
                if slot.material==material:
                    uses.append(dict(object=obj.name_full,mesh=obj.data.name_full,slot=slot_index,
                        slot_link=slot.link,polygons=[p.index for p in obj.data.polygons if p.material_index==slot_index]))
        records.append(dict(name=material.name_full,use_nodes=material.use_nodes,
                            library=material.library.filepath if material.library else None,
                            surface_render_method=material.surface_render_method,nodes=nodes,links=links,uses=uses))
    return records


def export(source,destination):
    source=pathlib.Path(source);destination=pathlib.Path(destination)
    digest=hashlib.sha256(source.read_bytes()).hexdigest()
    bpy.ops.wm.open_mainfile(filepath=str(source.resolve()),use_scripts=False)
    records=inventory()
    result=dict(schema='hone.material-inventory.v1',source_sha256=digest,
                blender=bpy.app.version_string,runtime_ready=False,materials=records)
    payload=json.dumps(result,ensure_ascii=False,separators=(',',':'),allow_nan=False).encode('utf8')
    with destination.open('xb') as stream:stream.write(payload)
    assert hashlib.sha256(source.read_bytes()).hexdigest()==digest
    return dict(source_sha256=digest,output_sha256=hashlib.sha256(payload).hexdigest(),
                materials=len(records),runtime_ready=False)


if __name__=='__main__':
    args=sys.argv[sys.argv.index('--')+1:]
    if len(args)!=2:raise SystemExit('expected blend and new output')
    print(json.dumps(export(*args)))
