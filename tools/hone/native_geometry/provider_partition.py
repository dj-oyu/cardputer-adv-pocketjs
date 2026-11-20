"""Offline, order-preserving 32-corner provider admission plan, not runtime data.

Each part maps local indices to the prepared corner-split arrays, NOT original
Blender vertex IDs or source loop IDs. Parts hold at most 64 triangles. Runtime must
not repartition or copy full meshes. Materials, transforms, skin and node identity
are not inferred; this plan alone must never authorize model registration.
"""
from consumer_plan import prepare


def partition_mesh(mesh, vertex_limit=32):
    if type(vertex_limit) is not int or not 3 <= vertex_limit <= 32:
        raise ValueError('provider vertex limit must be 3..32')
    vertices = mesh['positions']
    parts = []
    remap = {}
    source = []
    triangles = []
    source_triangles = []
    def flush():
        if triangles:
            parts.append(dict(prepared_vertices=source.copy(),
                              triangles=[t.copy() for t in triangles],
                              source_triangles=source_triangles.copy()))
    for tid, triangle in enumerate(mesh['triangles']):
        if len(triangle) != 3 or any(type(v) is not int or not 0 <= v < len(vertices) for v in triangle):
            raise ValueError('invalid triangle')
        if len(triangles) >= 64 or len(remap) + len(set(triangle).difference(remap)) > vertex_limit:
            flush(); remap = {}; source = []; triangles = []; source_triangles = []
        row = []
        for v in triangle:
            if v not in remap:
                remap[v] = len(source); source.append(v)
            row.append(remap[v])
        triangles.append(row); source_triangles.append(tid)
    flush()
    return parts


def plan(package):
    prepared = prepare(package)
    meshes = [partition_mesh(m) for m in prepared['meshes']]
    count = sum(map(len, meshes))
    return dict(runtime_ready=False, mesh_parts=meshes, geometry_nodes=count,
                model_node_limit=256, exceeds_geometry_node_budget=count > 256,
                remaining=['reserved hierarchy/nonmesh nodes', 'materials and alpha',
                           'node-local transforms and rig', 'morph control mapping',
                           'production provider binding and resource budget'])


if __name__ == '__main__':
    import argparse, json, pathlib
    parser = argparse.ArgumentParser()
    parser.add_argument('source'); parser.add_argument('output')
    args = parser.parse_args()
    result = plan(json.loads(pathlib.Path(args.source).read_text()))
    # A real-model mapping is private just like the source; never log its content.
    with pathlib.Path(args.output).open('x') as stream:
        json.dump(result, stream, separators=(',', ':'))
    print(json.dumps({k: v for k, v in result.items() if k != 'mesh_parts'}))
