"""Offline source-bound provider layout. Never grants runtime registration.

Geometry nodes bind source mesh transforms by index. Partition siblings reuse
that mesh's evaluated transform, not copies of its parent transform multiplied
again. Source parents/rig remain authoritative; no synthetic transform-only
parent per mesh is allocated here. Scene/rig evaluator integration is required.
"""
from consumer_plan import prepare
from provider_partition import partition_mesh


def binding_plan(package):
    prepared=prepare(package)
    nodes=[]
    for mid,(mesh,raw) in enumerate(zip(prepared['meshes'],package['meshes'])):
        slots=raw.get('material_slots')
        if not isinstance(slots,list) or not slots: raise ValueError('missing material slots')
        materials=[]
        for polygon in mesh['source_polygons']:
            material=raw['polygons'][polygon].get('material')
            if type(material) is not int or not 0<=material<len(slots):
                raise ValueError('invalid material slot')
            materials.append(material)
        # Flush on a material change: never reorder triangles across transparent
        # material runs. Reuse original prepared vertex indices in every part.
        start=0
        while start<len(materials):
            end=start+1
            while end<len(materials) and materials[end]==materials[start]:end+=1
            run=dict(positions=mesh['positions'],triangles=mesh['triangles'][start:end])
            for part in partition_mesh(run):
                part['source_triangles']=[t+start for t in part['source_triangles']]
                nodes.append(dict(source_mesh=mid,source_transform=mid,
                                  material_slot=materials[start],**part))
            start=end
    return dict(runtime_ready=False,nodes=nodes,geometry_nodes=len(nodes),
                model_node_limit=256,exceeds_geometry_node_budget=len(nodes)>256,
                remaining=['dynamic source/rig transform evaluator','preserve-volume skinning',
                           'shrinkwrap and constraints','normal/material/alpha semantics',
                           'total scene and runtime memory budget','production registration'])
