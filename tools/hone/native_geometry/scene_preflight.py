"""Aggregate source geometry requirements; never grants runtime admission."""
import collections
import json
import pathlib
import sys
from provider_binding import binding_plan


def inspect(package):
    plan = binding_plan(package)
    counts = collections.Counter()
    modifiers = collections.Counter()
    for mesh in package['meshes']:
        custom = mesh.get('has_custom_normals')
        sharp = mesh.get('sharp_edges')
        active = mesh.get('modifiers')
        if type(custom) is not bool or not isinstance(sharp, list) or not isinstance(active, list):
            raise ValueError('missing normal or modifier inventory')
        counts['meshes'] += 1
        counts['custom_normal_meshes'] += custom
        counts['sharp_edge_meshes'] += bool(sharp)
        counts['ngon_polygons'] += sum(len(poly['vertices']) > 4 for poly in mesh['polygons'])
        counts['vertices_over_four_weights'] += sum(len(row) > 4 for row in mesh['weights'])
        counts['shape_key_meshes'] += bool(mesh['shape_keys'])
        for mod in active:
            if not isinstance(mod, dict) or type(mod.get('viewport')) is not bool or type(mod.get('render')) is not bool or not isinstance(mod.get('type'), str):
                raise ValueError('incomplete modifier inventory')
            if mod['viewport']:
                modifiers[mod['type']] += 1
            if mod['viewport'] != mod['render']:
                counts['viewport_render_modifier_differences'] += 1
    n = plan['geometry_nodes']
    return dict(schema='hone.scene-preflight.v1', runtime_ready=False,
                source_sha256=package.get('source_sha256'), geometry_nodes=n,
                model_node_limit=256, remaining_slots_before_other_nodes=max(0, 256-n),
                geometry_exceeds_limit=n > 256, counts=dict(counts),
                viewport_modifiers=dict(modifiers),
                pending=['non-geometry node assignment and full node budget',
                         'modifier semantics and evaluation order', 'dynamic controls and rig',
                         'materials and alpha', 'memory budget and runtime admission'])


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('expected source package path')
    package = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf8'))
    print(json.dumps(inspect(package), indent=2, allow_nan=False))
