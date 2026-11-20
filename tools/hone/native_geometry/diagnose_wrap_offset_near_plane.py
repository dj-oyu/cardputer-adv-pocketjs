"""Synthetic black-box diagnostic, NOT an equivalence/admission test.
Run: python diagnose_wrap_offset_near_plane.py --blender /path/to/blender
No private model, external source code, or firmware is used. Never saves a blend.
"""
import argparse
import json
import math
import pathlib
import struct
import subprocess
import sys


def f32(value):
    return struct.unpack("f", struct.pack("f", value))[0]


def diagnose():
    import bpy
    from mathutils import Vector
    from mathutils.geometry import closest_point_on_tri, normal

    def obj(name, vertices, faces):
        mesh = bpy.data.meshes.new(name)
        mesh.from_pydata(vertices, [], faces)
        result = bpy.data.objects.new(name, mesh)
        bpy.context.collection.objects.link(result)
        return result

    bpy.ops.object.select_all(action="SELECT")
    bpy.ops.object.delete(use_global=False)
    source = obj("offset-source", [(0, 0, 0)], [])
    modifier = source.modifiers.new("offset-oracle", "SHRINKWRAP")
    modifier.wrap_method = "NEAREST_SURFACEPOINT"
    modifier.wrap_mode = "ON_SURFACE"

    def evaluate(offset):
        modifier.offset = offset
        source.data.update()
        source.update_tag()
        bpy.context.view_layer.update()
        graph = bpy.context.evaluated_depsgraph_get()
        graph.update()
        evaluated = source.evaluated_get(graph)
        mesh = evaluated.to_mesh()
        try:
            return tuple(mesh.vertices[0].co)
        finally:
            evaluated.to_mesh_clear()

    regions = {
        "tilted_interior": [(0.5, 0.75, 1.25), (1.6, 0.9, 1.4), (0.7, 2., 1.1)],
        "axis_vertex": [(0., 0., 0.), (4., 0., 0.), (0., 4., 0.)],
    }
    print("ORACLE", bpy.app.version_string)
    total = 0
    for region, positions in regions.items():
        vertices = [Vector(p) for p in positions]
        target = obj(region, vertices, [(0, 1, 2)])
        modifier.target = target
        face = normal(*vertices)
        center = sum(vertices, Vector()) / 3
        for side in (-1, 1):
            for offset in (-0.0025, 0.0025, 0.0001, 0.001, 0.01, 0.1):
                for exponent in range(-1, -9, -1):
                    distance = 10. ** exponent
                    query = (center + face * (side * distance) if region == "tilted_interior"
                             else Vector((-distance, -distance, side * distance)))
                    source.data.vertices[0].co = query
                    query = tuple(source.data.vertices[0].co)
                    actual = evaluate(offset)
                    effective_offset = modifier.offset
                    hit = evaluate(0.)
                    repeat = evaluate(offset)
                    assert actual == repeat, "offset changes must not depend on prior evaluation"
                    assert all(math.isfinite(x) for x in (*query, *actual, *hit))
                    delta = [f32(query[k] - hit[k]) for k in range(3)]
                    length = f32(math.sqrt(f32(f32(f32(delta[0]**2) + f32(delta[1]**2))
                                                 + f32(delta[2]**2))))
                    variants = {}
                    if length:
                        ratio = f32(effective_offset / length)
                        inv = f32(1. / length)
                        variants["normalize_scale"] = [f32(hit[k] + f32(effective_offset * f32(delta[k] * inv))) for k in range(3)]
                        variants["ratio_scale"] = [f32(hit[k] + f32(delta[k] * ratio)) for k in range(3)]
                        variants["lerp"] = [f32(f32(hit[k] * f32(1. - ratio)) + f32(query[k] * ratio)) for k in range(3)]
                        variants["query_delta"] = [f32(query[k] + f32(delta[k] * f32(ratio - 1.))) for k in range(3)]
                        if region == "tilted_interior":
                            # Independent plane formula, interior only; not a proposed fallback.
                            terms = [f32(f32(query[k] - vertices[0][k]) * face[k]) for k in range(3)]
                            signed = f32(f32(terms[0] + terms[1]) + terms[2])
                            move = f32(side * effective_offset - signed)
                            variants["signed_plane"] = [f32(query[k] + f32(face[k] * move)) for k in range(3)]
                    helper_hit = closest_point_on_tri(Vector(query), *vertices)
                    errors = {name: max(abs(pred[k] - actual[k]) for k in range(3))
                              for name, pred in variants.items()}
                    assert all(math.isfinite(x) for x in errors.values())
                    print("OFFSET_DIAGNOSTIC", json.dumps(dict(region=region, side=side,
                        offset=effective_offset, offset_sign=1 if offset > 0 else -1, exponent=exponent,
                        zero_length=length == 0., errors=errors, actual_distance=math.sqrt(sum((actual[k]-hit[k])**2 for k in range(3))), query_distance=length,
                        zero_hit_helper_error=max(abs(hit[k] - helper_hit[k]) for k in range(3)))))
                    total += 1
    assert total == 192
    print("DIAGNOSTIC_COMPLETE", total, "cases; no equivalence claim")


if __name__ == "__main__":
    if "--oracle" in sys.argv:
        diagnose()
    else:
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("--blender", required=True)
        args = parser.parse_args()
        subprocess.run([args.blender, "--background", "--factory-startup", "--python-exit-code", "1",
                        "--python", str(pathlib.Path(__file__).resolve()), "--", "--oracle"], check=True)
