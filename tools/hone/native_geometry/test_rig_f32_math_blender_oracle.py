"""Synthetic Blender oracle for opt-in rig math; never loads private data.
Host: python test_rig_f32_math_blender_oracle.py --blender /path/to/blender
"""
import ctypes as c
import json
import pathlib
import random
import sys


def oracle(library):
    import bpy
    from mathutils import Euler
    lib = c.CDLL(str(library))
    fp = c.POINTER(c.c_float)
    lib.euler.argtypes = [fp, fp]
    lib.quaternion.argtypes = [fp, fp]
    lib.position.argtypes = [fp, fp, c.c_size_t, c.POINTER(c.c_uint32), fp, c.c_size_t, fp]
    rng = random.Random(38129)
    euler_components = quaternion_components = rejected = 0
    for trial in range(4000):
        limit = .25 if trial < 2000 else 3.1
        angles = (c.c_float*3)(*(rng.uniform(-limit, limit) for _ in range(3)))
        reference = Euler(tuple(angles), "XYZ").to_matrix()
        matrix = (c.c_float*9)()
        assert lib.euler(angles, matrix)
        assert list(matrix) == [reference[r][k] for r in range(3) for k in range(3)]
        euler_components += 9
        output = (c.c_float*4)(11, 22, 33, 44)
        admitted = all(reference[k][k] >= 0 for k in range(3))
        accepted = lib.quaternion(matrix, output)
        assert bool(accepted) == admitted
        if admitted:
            assert list(output) == list(reference.to_quaternion())
            quaternion_components += 4
        else:
            assert list(output) == [11, 22, 33, 44]
            rejected += 1

    # Independent synthetic Armature modifier, no constraints or hierarchy.
    # Evaluated pose matrices supply the palette to isolate DQ point arithmetic.
    # This does NOT establish native palette/driver/space integration.
    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    armature = bpy.data.armatures.new("rig-math-oracle")
    arm = bpy.data.objects.new("rig-math-oracle", armature)
    bpy.context.collection.objects.link(arm)
    bpy.context.view_layer.objects.active = arm
    arm.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    for name, head in (("joint-a", (0, 0, 0)), ("joint-b", (.3, .2, 0))):
        bone = armature.edit_bones.new(name)
        bone.head = head
        bone.tail = (head[0], head[1]+1, head[2])
    bpy.ops.object.mode_set(mode="OBJECT")
    mesh = bpy.data.meshes.new("rig-math-points")
    mesh.from_pydata([tuple(rng.uniform(-2, 2) for _ in range(3)) for _ in range(64)], [], [])
    source = bpy.data.objects.new("rig-math-points", mesh)
    bpy.context.collection.objects.link(source)
    groups = [source.vertex_groups.new(name=name) for name in ("joint-a", "joint-b")]
    for index in range(64):
        weight = 1.0 if index % 4 == 0 else 0.0 if index % 4 == 1 else rng.uniform(.05, .95)
        if weight: groups[0].add([index], weight, "REPLACE")
        if weight != 1: groups[1].add([index], 1-weight, "REPLACE")
    modifier = source.modifiers.new("rig-math-oracle", "ARMATURE")
    modifier.object = arm
    modifier.use_deform_preserve_volume = True
    for bone in arm.pose.bones: bone.rotation_mode = "XYZ"
    maximum = 0.0
    for _ in range(64):
        for bone in arm.pose.bones:
            bone.rotation_euler = tuple(rng.uniform(-.25, .25) for _ in range(3))
            bone.location = tuple(rng.uniform(-.1, .1) for _ in range(3))
        arm.update_tag()
        bpy.context.view_layer.update()
        graph = bpy.context.evaluated_depsgraph_get()
        graph.update()
        evaluated_arm = arm.evaluated_get(graph)
        palette = []
        for name in ("joint-a", "joint-b"):
            transform = evaluated_arm.pose.bones[name].matrix @ armature.bones[name].matrix_local.inverted()
            palette.extend([*transform.to_quaternion(), *transform.translation])
        joints = (c.c_float*14)(*palette)
        evaluated = source.evaluated_get(graph)
        evaluated_mesh = evaluated.to_mesh()
        try:
            for vertex, reference in zip(mesh.vertices, evaluated_mesh.vertices):
                influences = list(vertex.groups)
                indices = (c.c_uint32*len(influences))(*(g.group for g in influences))
                weights = (c.c_float*len(influences))(*(g.weight for g in influences))
                output = (c.c_float*3)()
                assert lib.position((c.c_float*3)(*vertex.co), joints, 2, indices, weights, len(influences), output)
                error = max(abs(a-b) for a, b in zip(output, reference.co))
                maximum = max(maximum, error)
                assert error < 1e-5, error
        finally:
            evaluated.to_mesh_clear()
    print("RIG_F32_BLENDER_ORACLE", json.dumps({"blender": bpy.app.version_string,
          "euler_components_equal": euler_components, "quaternion_components_equal": quaternion_components,
          "out_of_domain_rejected": rejected, "dq_vertices": 4096, "dq_max_component_error": maximum,
          "runtime_admitted": False}))


if __name__ == "__main__":
    if "--" in sys.argv:
        oracle(pathlib.Path(sys.argv[sys.argv.index("--")+1]).resolve())
    else:
        import argparse
        import subprocess
        import tempfile
        from test_rig_f32_math_candidate import build_library
        parser = argparse.ArgumentParser()
        parser.add_argument("--blender", required=True)
        args = parser.parse_args()
        with tempfile.TemporaryDirectory() as directory:
            library = build_library(directory)
            subprocess.run([args.blender, "--background", "--factory-startup", "--python-exit-code", "1",
                            "--python", str(pathlib.Path(__file__).resolve()), "--", str(library)], check=True)
