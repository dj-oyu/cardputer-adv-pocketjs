"""Aggregate frame array accounting; not target allocator/stack measurement.

Private inputs are read-only; reports contain counts/bytes only. All quantities
use explicit float32/float64 widths, excluding ABI descriptors and alignment.
Normal workspace may be shared only across sequential whole-mesh evaluations;
pending positions/normals stay alive until provider evaluation has completed.
"""
import argparse, json, pathlib
from provider_binding import binding_plan


def inspect(package, non_geometry_nodes=0):
    if type(non_geometry_nodes) is not int or non_geometry_nodes<0:
        raise ValueError('non-geometry node count')
    plan=binding_plan(package)
    vertices=[len(m['vertices']) for m in package['meshes']]
    corners=[sum(len(p['vertices']) for p in m['polygons']) for m in package['meshes']]
    keys=[len(m['shape_keys']['blocks']) if m['shape_keys'] else 0 for m in package['meshes']]
    positions=12*sum(vertices);normals=12*sum(corners)
    independent=24*sum(vertices);shared=24*max(vertices,default=0)
    nodes=plan['geometry_nodes']+non_geometry_nodes
    return dict(schema='hone.frame-array-budget.v1',runtime_ready=False,
        counts=dict(meshes=len(vertices),source_vertices=sum(vertices),packed_polygon_corners=sum(corners),geometry_nodes=plan['geometry_nodes'],declared_non_geometry_nodes=non_geometry_nodes,total_declared_nodes=nodes),
        node_limit=256,declared_nodes_exceed_limit=nodes>256,
        bytes=dict(pending_source_positions=positions,pending_packed_normals=normals,
            normal_workspace_per_mesh_total=independent,normal_workspace_sequential=shared,
            normal_workspace_saved=independent-shared,
            pending_geometry_plus_shared_normal_work=positions+normals+shared,
            dense_morph_masks=4*sum(v*k for v,k in zip(vertices,keys)),
            provider_slot_float_payload=32*6*4,provider_index_plan_uint32_payload=32*2*4,
            measured_wedge_lut_float_payload=65*4),
        excluded=['batch job and provider descriptors','model arena and scene transforms','ready and pending snapshots','materials/alpha packets and bounds','rig palette and driver arrays','modifier work and dependencies','cold geometry placement','LCD/DMA buffers','ABI padding and allocator overhead','call stack and libm stack'],
        conditions=['complete scene node assignment still required','one synchronous normal job at a time','workspace disjoint from all inputs/outputs','all pending mesh positions/normals retained through provider evaluation','ready presentation owns its independent snapshot'])


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('source',type=pathlib.Path);p.add_argument('--non-geometry-nodes',type=int,default=0);a=p.parse_args()
    print(json.dumps(inspect(json.loads(a.source.read_text(encoding='utf8')),a.non_geometry_nodes),indent=2))
