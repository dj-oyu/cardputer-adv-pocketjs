"""Capture Blender driver bindings as private source data, not executable code.

Run inside Blender with -- <blend> <new-output>. Only source identifiers,
expressions, binding paths and current scalar values are recorded. This does not
evaluate drivers, infer control semantics or authorize runtime admission.
"""
import bpy, hashlib, json, math, pathlib, sys


def identity(value):
    if value is None: return None
    result=dict(type=value.bl_rna.identifier, name=value.name_full)
    if isinstance(value,bpy.types.NodeTree) and value.is_embedded_data:
        # Embedded shader trees can share the same name across materials.
        # Retain the owner rather than inventing a globally unique tree name.
        owners=[m for m in bpy.data.materials if m.node_tree==value]
        result['embedded_in']=[dict(type=m.bl_rna.identifier,name=m.name_full) for m in owners]
    return result


def scalar(target):
    if target.id is None or not target.data_path:
        return dict(status='missing_target')
    try: value=target.id.path_resolve(target.data_path)
    except (ValueError,TypeError,AttributeError): return dict(status='unresolved')
    if type(value) not in (int,float,bool): return dict(status='non_scalar')
    if isinstance(value,float) and not math.isfinite(value): return dict(status='non_finite')
    return dict(status='ok',value=value)


def inventory():
    blocks=[*bpy.data.objects,*bpy.data.shape_keys,*bpy.data.materials,*bpy.data.node_groups]
    blocks.extend(m.node_tree for m in bpy.data.materials if m.node_tree)
    blocks={b.as_pointer():b for b in blocks}.values()
    records=[]
    for owner in blocks:
        animation=owner.animation_data
        if animation is None: continue
        for curve in animation.drivers:
            driver=curve.driver
            modifiers=[]
            for modifier in curve.modifiers:
                entry=dict(type=modifier.type,mute=modifier.mute,
                    use_restricted_range=modifier.use_restricted_range,
                    use_influence=modifier.use_influence)
                if modifier.type=='GENERATOR':
                    entry.update(mode=modifier.mode,poly_order=modifier.poly_order,
                        coefficients=list(modifier.coefficients),use_additive=modifier.use_additive)
                modifiers.append(entry)
            variables=[]
            for var in driver.variables:
                targets=[]
                for target in var.targets:
                    targets.append(dict(id_type=target.id_type,id=identity(target.id),
                        data_path=target.data_path,bone_target=target.bone_target,
                        transform_type=target.transform_type,transform_space=target.transform_space,
                        rotation_mode=target.rotation_mode,current=scalar(target)))
                variables.append(dict(name=var.name,type=var.type,targets=targets))
            records.append(dict(owner=identity(owner),data_path=curve.data_path,
                array_index=curve.array_index,mute=curve.mute,curve_valid=curve.is_valid,
                driver_valid=driver.is_valid,keyframe_count=len(curve.keyframe_points),
                fcurve_modifiers=modifiers,type=driver.type,
                expression=driver.expression,variables=variables))
    return records


def export(source,destination):
    source=pathlib.Path(source); destination=pathlib.Path(destination)
    digest=hashlib.sha256(source.read_bytes()).hexdigest()
    bpy.ops.wm.open_mainfile(filepath=str(source.resolve()),use_scripts=False)
    records=inventory()
    result=dict(schema='hone.driver-inventory.v1',source_sha256=digest,
                blender=bpy.app.version_string,runtime_ready=False,drivers=records)
    encoded=json.dumps(result,ensure_ascii=False,separators=(',',':'),allow_nan=False).encode('utf8')
    with destination.open('xb') as stream: stream.write(encoded)
    assert hashlib.sha256(source.read_bytes()).hexdigest()==digest
    return dict(drivers=len(records),source_sha256=digest,output_sha256=hashlib.sha256(encoded).hexdigest(),runtime_ready=False)


if __name__=='__main__':
    args=sys.argv[sys.argv.index('--')+1:]
    if len(args)!=2: raise SystemExit('expected blend and new output')
    print(json.dumps(export(*args)))
