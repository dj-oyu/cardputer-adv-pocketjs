"""Synthetic Blender black-box oracle; no model or private fixture is used.
Host: python test_affine_f32_blender_oracle.py --blender /path/to/blender
The supplied Blender binary is the oracle; version is printed, not generalized.
"""
import ctypes as c
import json
import pathlib
import random
import sys


def oracle(library):
    import bpy
    from mathutils import Matrix, Vector
    from mathutils.geometry import closest_point_on_tri
    lib = c.CDLL(str(library))
    fp = c.POINTER(c.c_float)
    lib.relative.argtypes = [fp, fp, fp]
    lib.point.argtypes = [fp, fp, fp]

    def flatten(matrix):
        return (c.c_float*12)(*(matrix[r][k] for r in range(3) for k in range(4)))

    def point(matrix, vertex):
        result = (c.c_float*3)()
        assert lib.point(matrix, (c.c_float*3)(*vertex), result)
        return Vector(result)

    def mesh_object(name, vertices, faces):
        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata(vertices, [], faces)
        obj = bpy.data.objects.new(name, mesh)
        bpy.context.collection.objects.link(obj)
        return obj

    # Runs in a fresh factory-startup process and never saves a blend file.
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    source = mesh_object("affine-source", [(0, 0, 0)], [])
    target = mesh_object("affine-target", [(0, 0, 0), (10, 0, 0), (0, 10, 0)], [(0, 1, 2)])
    parent = bpy.data.objects.new("affine-common-parent", None)
    bpy.context.collection.objects.link(parent)
    modifier = source.modifiers.new("affine-oracle", "SHRINKWRAP")
    modifier.target = target
    modifier.wrap_method = "NEAREST_SURFACEPOINT"
    modifier.wrap_mode = "ON_SURFACE"
    modifier.offset = 0
    rng = random.Random(91627)
    components = 0
    for same_parent in (False, True):
        source.parent = parent if same_parent else None
        target.parent = parent if same_parent else None
        source.matrix_parent_inverse = Matrix.Identity(4)
        target.matrix_parent_inverse = Matrix.Identity(4)
        for region in ("vertex", "interior"):
            for _ in range(256):
                for obj in (source, target, parent):
                    obj.rotation_euler = tuple(rng.uniform(-2, 2) for _ in range(3))
                    obj.location = tuple(rng.uniform(-3, 3) for _ in range(3))
                anchor = Vector(tuple(rng.uniform(-2, 2) for _ in range(3)))
                for vertex, delta in zip(target.data.vertices, ((0, 0, 0), (10, 0, 0), (0, 10, 0))):
                    vertex.co = anchor + Vector(delta)
                target.data.update()
                target.update_tag()
                bpy.context.view_layer.update()
                delta = Vector((-2, -2, 1)) if region == "vertex" else Vector(tuple(rng.uniform(.1, 2) for _ in range(3)))
                source.data.vertices[0].co = (source.matrix_world.inverted() @ target.matrix_world) @ (anchor + delta)
                source.data.update()
                source.update_tag()
                bpy.context.view_layer.update()
                graph = bpy.context.evaluated_depsgraph_get()
                graph.update()
                evaluated = source.evaluated_get(graph)
                mesh = evaluated.to_mesh()
                reference = list(mesh.vertices[0].co)
                evaluated.to_mesh_clear()
                maps = (c.c_float*24)()
                assert lib.relative(flatten(target.matrix_world), flatten(source.matrix_world), maps)
                forward = (c.c_float*12)(*maps[:12])
                reverse = (c.c_float*12)(*maps[12:])
                query = point(forward, source.data.vertices[0].co)
                hit = target.data.vertices[0].co.copy() if region == "vertex" else closest_point_on_tri(query, *(v.co for v in target.data.vertices))
                actual = point(reverse, hit)
                assert list(actual) == reference, {"same_parent": same_parent, "region": region,
                                                  "max_error": max(abs(a-b) for a, b in zip(actual, reference))}
                components += 3
    print("AFFINE_F32_ORACLE", json.dumps({"blender": bpy.app.version_string,
          "transforms": components//3, "components_equal": components,
          "common_parent_and_independent": True, "vertex_and_interior": True,
          "runtime_admitted": False}))


if __name__ == "__main__":
    if "--" in sys.argv:
        oracle(pathlib.Path(sys.argv[sys.argv.index("--")+1]).resolve())
    else:
        import argparse
        import subprocess
        import tempfile
        from test_affine_f32_candidate import build_library
        parser = argparse.ArgumentParser()
        parser.add_argument("--blender", required=True)
        args = parser.parse_args()
        with tempfile.TemporaryDirectory() as directory:
            library = build_library(directory)
            subprocess.run([args.blender, "--background", "--factory-startup", "--python-exit-code", "1",
                            "--python", str(pathlib.Path(__file__).resolve()), "--", str(library)], check=True)
