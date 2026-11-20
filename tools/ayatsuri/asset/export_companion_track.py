"""Export the authored Companion rig through Blender's evaluated mesh API.

blender -b fox.blend --disable-autoexec --python-exit-code 1 \
  --python export_companion_track.py -- OUTPUT_DIRECTORY
"""

from colorsys import hls_to_rgb, rgb_to_hls
import hashlib
import json
from math import cos, pi
from pathlib import Path
import sys

import bpy
from mathutils import Vector
from mathutils.bvhtree import BVHTree
import numpy as np


WIDTH, HEIGHT = 240, 135
MOTION_FRAMES = 168
PERIOD_MS = 48
KEYS = sorted(set(range(0, MOTION_FRAMES + 1, 8)) | {19, 21, 23, 52, 60, 70, 98, 106, 146, 149, 151, 153})
INSPECTION = (
    ("Joy", 0.40, .55, {"mouth_open_smile": 1, "happy_eyes": 1}, {}),
    ("Surprise", 0.19, .43, {"mouth_open_smile": .75, "eye_blank_both": .32, "brow_worry": .18}, {}),
    ("Concern", 0.60, .61, {"brow_worry": .95, "gaze_x": -.50}, {"sweat": 1}),
    ("Blink", 0, .17, {"blink_both": 1}, {}),
    ("Inhale", .22, 1, {}, {}),
    ("Glance", .36, .42, {"gaze_x": .8}, {"confusion": 1}),
)
DEFAULT_FACE = ("gaze_x", "blink_both", "blink_left", "blink_right", "brow_worry", "mouth_smile",
                "eye_blank_both", "eye_blank_left", "eye_blank_right", "mouth_open_smile", "happy_eyes")
DEFAULT_MARKS = ("sweat", "confusion", "anger")
KEY_DIRECTION = (-.42, -.61, .68)
VIEW_DIRECTION = (-.31, -.95, .16)


def smooth(value):
    value = np.clip(value, 0, 1)
    return value * value * (3 - 2 * value)


def pulse(t, first, full, release, end):
    return float(smooth((t - first) / (full - first)) * smooth((end - t) / (end - release)))


def motion(frame):
    phase = frame / MOTION_FRAMES
    turn = .5 - .5 * cos(2 * pi * phase)
    breath = .5 - .5 * cos(4 * pi * phase)
    joy = pulse(frame, 43, 60, 70, 82)
    concern = pulse(frame, 83, 98, 111, 123)
    glance = pulse(frame, 118, 132, 142, 157)
    blink = max(pulse(frame, 19, 21, 21, 24), pulse(frame, 146, 149, 151, 154))
    face = {"gaze_x": .80 * glance - .44 * concern, "blink_both": blink,
            "brow_worry": .95 * concern, "mouth_smile": .70 * joy,
            "mouth_open_smile": joy, "happy_eyes": joy}
    marks = {"sweat": concern, "confusion": .85 * glance}
    return (f"motion-{frame:03d}", turn, breath, face, marks)


class EyeMaterial:
    """Evaluate the authored eye texture/mask nodes at triangle UV samples."""

    def __init__(self, material, textures):
        self.nodes = material.node_tree.nodes
        self.side = "left" if "-1" in material.name else "right"
        self.textures = textures
        self.gaze_factor = float(next(driver.driver.expression for driver in material.node_tree.animation_data.drivers
                                      if 'Vector Math' in driver.data_path).split('*')[-1])

    def sample(self, uv, face):
        blink = max(face.get("blink_both", 0), face.get("blink_" + self.side, 0))
        blank = max(face.get("eye_blank_both", 0), face.get("eye_blank_" + self.side, 0))
        gaze = face.get("gaze_x", 0) * self.gaze_factor
        cache = {}

        def read(socket):
            if socket.is_linked:
                link = socket.links[0]
                return evaluate(link.from_node, link.from_socket)
            return np.asarray(socket.default_value)

        def evaluate(node, socket):
            key = (node.name, socket.identifier)
            if key in cache:
                return cache[key]
            if node.type == 'TEX_COORD':
                out = uv
            elif node.type == 'VALUE':
                out = blink
            elif node.type == 'VECT_MATH' and node.operation == 'ADD':
                out = read(node.inputs[0]) + (gaze, 0)
            elif node.type == 'TEX_IMAGE':
                out = sample_image(self.textures[node.image.name], read(node.inputs[0]), node.extension == 'REPEAT')
            elif node.type == 'SEPARATE_COLOR':
                out = read(node.inputs[0])[..., ('Red', 'Green', 'Blue').index(socket.name)]
            elif node.type == 'SEPXYZ':
                out = read(node.inputs[0])[..., ('X', 'Y').index(socket.name)]
            elif node.type == 'MATH':
                a, b, c = (read(item) for item in node.inputs[:3])
                op = node.operation
                if op == 'MULTIPLY_ADD':
                    out = a * b + c
                else:
                    out = np.asarray(OPERATIONS[op](a) if op == 'ABSOLUTE' else OPERATIONS[op](a, b), dtype=np.float32)
                if node.use_clamp:
                    out = np.clip(out, 0, 1)
            elif node.type == 'MIX_RGB' and node.blend_type == 'MIX':
                fac = blank if node.name.startswith('Comic blank') else np.clip(read(node.inputs[0]), 0, 1)
                out = read(node.inputs[1]) * (1 - np.asarray(fac)[..., None]) + read(node.inputs[2]) * np.asarray(fac)[..., None]
            else:
                raise ValueError(f"Unsupported eye shader node {node.name}: {node.type}")
            cache[key] = out
            return out

        colors = read(self.nodes['Principled BSDF'].inputs['Base Color'])[..., :3]
        shader = self.nodes['Mix Shader']
        assert shader.inputs[2].links[0].from_node.type == 'BSDF_TRANSPARENT'
        return colors, np.broadcast_to(1 - np.clip(read(shader.inputs[0]), 0, 1), uv.shape[:-1])


OPERATIONS = {'ADD': np.add, 'SUBTRACT': np.subtract, 'MULTIPLY': np.multiply,
              'LESS_THAN': np.less, 'GREATER_THAN': np.greater, 'MAXIMUM': np.maximum,
              'MINIMUM': np.minimum, 'ABSOLUTE': np.abs}


def sample_image(pixels, coordinates, wrap):
    size = np.asarray(pixels.shape[1::-1])
    coordinates = coordinates * size - .5
    base = np.floor(coordinates).astype(int)
    spread = coordinates - base
    result = np.zeros((*coordinates.shape[:-1], 4), np.float32)
    for x, y in ((0, 0), (1, 0), (0, 1), (1, 1)):
        index = (base + (x, y)) % size if wrap else np.clip(base + (x, y), 0, size - 1)
        weight = (spread[..., 0] if x else 1 - spread[..., 0]) * (spread[..., 1] if y else 1 - spread[..., 1])
        result += pixels[index[..., 1], index[..., 0]] * weight[..., None]
    return result


def linear_to_srgb(rgb):
    return np.where(rgb < .00313, rgb * 12.92, 1.055 * np.maximum(rgb, 0) ** (1 / 2.4) - .055)


def unit(values):
    return values / np.maximum(np.linalg.norm(values, axis=-1, keepdims=True), 1e-8)


def coat_colors(colors, normal, projected, eye_centers, face_surface):
    # A large elevated key and colored bounce leave a blue-violet bias in shade.
    key = np.clip(normal @ unit(np.array(KEY_DIRECTION)), 0, 1)
    fill = np.clip(normal @ unit(np.array([.53, -.82, .20])), 0, 1)
    rim = np.clip(normal @ unit(np.array([.24, .78, .58])), 0, 1)
    base = linear_to_srgb(colors)
    h, light, sat = rgb_to_hls(*base)
    shadow = np.asarray(hls_to_rgb((h + .50) % 1, light * .58, max(.42, sat * .82)))
    mix = (.25 * (1 - smooth(key + .28 * fill)))[..., None]
    shaded = base * (.72 + .20 * key + .07 * fill + .07 * rim)[..., None]
    shaded = shaded * (1 - mix) + shadow * mix
    if face_surface:
        # Fade the warmer frontal fill away from each eye; keep the muzzle and
        # outer head in the same lighting field to avoid a hard color seam.
        dist = np.minimum.reduce([np.linalg.norm((projected[:, :2] - eye[:2]) / (.34, .27), axis=-1)
                                  for eye in eye_centers])
        forward = smooth((-.25 - normal[:, 1]) / .65)
        weight = (.52 * (1 - smooth((dist - .36) / 1.05)) * forward)[:, None]
        shaded = shaded * (1 - weight) + base * (.89 + .08 * key)[:, None] * weight
        glint = (.13 * (1 - smooth((dist - .2) / 1.38)) * smooth((key - .35) / .55) * forward)[:, None]
        shaded = shaded * (1 - glint) + np.minimum(1, base * (1.20, 1.12, 1.17)) * glint
    return np.clip(shaded, 0, 1)


def extract(scene, record, world_to_camera, textures, topology):
    label, action_phase, breath_amount, face, marks = record
    frame = scene.frame_start + (scene.frame_end - scene.frame_start) * action_phase
    scene.frame_set(int(frame), subframe=frame % 1)
    for name, defaults, values in (("Gaze material controls", DEFAULT_FACE, face), ("Manga marks controls", DEFAULT_MARKS, marks)):
        ob = scene.objects[name]
        for prop in defaults:
            ob[prop] = float(values.get(prop, 0))
        ob.update_tag()
    breath = scene.objects['Breathing']
    breath['Cycle'] = float(breath_amount)
    breath['Tension'] = .23 + .63 * max(face.get('mouth_open_smile', 0), face.get('brow_worry', 0))
    breath.update_tag()
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    camera_rot, camera_offset = world_to_camera[:3, :3], world_to_camera[:3, 3]
    eye_centers = [camera_rot @ np.array(scene.objects[f'R99 eye {side}'].evaluated_get(dg).matrix_world.translation)
                   + camera_offset for side in (-1, 1)]
    meshes = {}
    for obj in scene.objects:
        if obj.type != 'MESH' or not len(obj.data.polygons):
            continue
        ev = obj.evaluated_get(dg)
        visible = not ev.hide_render and max(abs(v) for v in ev.scale) >= 1e-4
        mesh = ev.to_mesh(preserve_all_data_layers=True, depsgraph=dg)
        try:
            mesh.calc_loop_triangles()
            if not len(mesh.loop_triangles):
                continue
            world = np.array([ev.matrix_world @ vert.co for vert in mesh.vertices], dtype=np.float64)
            camera = world @ camera_rot.T + camera_offset
            # Use one diagonal across the animation. Blender's tessellator
            # can choose another diagonal as the rig deforms a quad.
            layout = tuple(tuple(p.vertices) for p in mesh.polygons)
            if obj.name not in topology:
                topology[obj.name] = (layout, np.asarray([tuple(t.vertices) for t in mesh.loop_triangles]),
                                      np.asarray([t.material_index for t in mesh.loop_triangles]),
                                      np.asarray([tuple(t.loops) for t in mesh.loop_triangles]))
            reference, triangle, material_ids, loops = topology[obj.name]
            assert layout == reference, f'Modifier changed polygon topology: {obj.name}'
            colors = np.full((len(triangle), 3), .5)
            keep = np.full(len(triangle), visible)
            for i, material in enumerate(mesh.materials):
                if material is None:
                    continue
                which = np.flatnonzero(material_ids == i)
                if not len(which):
                    continue
                name = material.name
                if name.startswith('Gaze eye'):
                    uv_data = mesh.uv_layers.active.data
                    uv = np.array([[uv_data[loop].uv for loop in loops[j]] for j in which])
                    # Three interior texture samples retain pupil contrast on
                    # the authored eye geometry without increasing triangles.
                    sample = uv.mean(axis=-2, keepdims=True) * .36 + uv * .64
                    rgb, alpha = EyeMaterial(material, textures).sample(sample, face)
                    keep[which] &= np.mean(alpha, axis=-1) >= .5
                    colors[which] = np.sum(rgb * alpha[..., None], axis=-2) / np.maximum(np.sum(alpha, axis=-1)[..., None], 1)
                elif name.startswith('Blink upper lid'):
                    side = 'left' if '-1' in name else 'right'
                    keep[which] &= max(face.get('blink_both', 0), face.get('blink_' + side, 0)) >= .13
                    colors[which] = (.093, .041, .068)
                elif name.startswith('Manga') or name.startswith('R99') or 'mouth' in name.lower() or 'smile' in name.lower():
                    colors[which] = linear_to_srgb(np.array(material.diffuse_color[:3]))
                else:
                    a, b, c = world[triangle[which]].transpose((1, 0, 2))
                    normals = unit(np.cross(b - a, c - a))
                    centers = camera[triangle[which]].mean(axis=1)
                    col = np.array(material.diffuse_color[:3])
                    colors[which] = coat_colors(col, normals, centers, eye_centers, obj.name.startswith('head87'))
            meshes[obj.name] = (camera, triangle, keep, colors)
        finally:
            ev.to_mesh_clear()
    return meshes


def bake_shadow_pattern(meshes, camera):
    """Bake a soft key occlusion pattern on the new neutral surface topology."""
    casters = {name: mesh for name, mesh in meshes.items()
               if not name.startswith(('R99', 'Happy eye', 'Readable', 'Manga'))}
    points, faces = [], []
    offset = 0
    for vertices, triangles, visible, _ in casters.values():
        points.extend(vertices.tolist())
        faces.extend((triangles[visible] + offset).tolist())
        offset += len(vertices)
    tree = BVHTree.FromPolygons(points, faces, all_triangles=True)
    light = np.asarray(KEY_DIRECTION)
    samples = [Vector(unit(camera[:3, :3] @ (light + np.asarray(offset))))
               for offset in ((-.11, 0, .02), (0, 0, 0), (.11, 0, -.02))]
    pattern = {}
    for name, (vertices, triangles, visible, _) in casters.items():
        a, b, c = vertices[triangles].transpose((1, 0, 2))
        normals = unit(np.cross(b - a, c - a))
        colors = np.zeros(len(triangles), np.float32)
        face_fill = name.startswith('head87')
        # Keep the front of the face in the soft fill. Its cast-shadow accent
        # belongs on the sides and underside, away from the eyes.
        fill = 1 - .80 * smooth((normals[:, 2] - .10) / .67) if face_fill else np.ones(len(triangles))
        for index in np.flatnonzero(visible):
            normal = Vector(normals[index])
            origin = Vector((a[index] + b[index] + c[index]) / 3) + normal * .006
            count = sum(normal.dot(direction) > .08 and tree.ray_cast(origin, direction, 3.5)[0] is not None
                        for direction in samples)
            colors[index] = count / len(samples) * fill[index]
        pattern[name] = colors
    return pattern


def apply_shadow_pattern(colors, weight):
    # A tinted bounce remains in covered regions. Blend in display color to
    # preserve the cloth-like whites and keep the fur shadows out of black.
    weight = (.19 * weight)[..., None]
    return colors * (1 - weight) + colors * (.50, .63, .96) * weight


def color565(colors):
    quant = np.floor(np.clip(colors, 0, 1) * (31, 63, 31) + .5).astype(np.uint16)
    return (quant[..., 0] << 11) | (quant[..., 1] << 5) | quant[..., 2]


def schedule():
    poses = [(i, i, 0) for i in range(len(INSPECTION))]
    gap = len(INSPECTION)
    for frame in range(MOTION_FRAMES):
        idx = int(np.searchsorted(KEYS, frame, side='right')) - 1
        blend = round(255 * (frame - KEYS[idx]) / (KEYS[idx + 1] - KEYS[idx]))
        poses.append((gap + idx, gap + idx + 1, blend))
    return poses


def rows(name, lines):
    return name + '={\n' + '\n'.join(lines) + '\n};\n'


def emit(output, raw, triangles, visible, colors, poses, source_sha):
    base = raw[len(INSPECTION)].astype(np.int32)
    displacement = raw.astype(np.int32) - base
    steps = np.maximum(1, (np.max(np.abs(displacement), axis=0) + 126) // 127)
    delta = np.rint(displacement / steps).astype(np.int8)
    pool = [(1, (0,) * len(raw))]
    channel_map = {pool[0]: 0}
    channels = np.zeros(base.shape, dtype=np.int32)
    for v in range(len(base)):
        for axis in range(3):
            seq = tuple(delta[:, v, axis])
            if any(seq):
                item = (int(steps[v, axis]), seq)
                channels[v, axis] = channel_map.setdefault(item, len(channel_map))
                if channels[v, axis] == len(pool):
                    pool.append(item)
    color_map, color_pool, groups, group_pool = {}, [(0,) * len(raw)], {}, []
    material_refs, visibility_refs = [], []
    for color, enabled in zip(colors.T, visible.T):
        active = np.nonzero(enabled)[0]
        assert len(active)
        values = tuple(color[np.array([min(active, key=lambda a: abs(int(a) - i)) if not flag else i
                                        for i, flag in enumerate(enabled)])])
        c = 0
        if len(set(values)) > 1:
            c = color_map.setdefault(values, len(color_map) + 1)
            if c == len(color_pool):
                color_pool.append(values)
        material_refs.append((values[0], c))
        flags = tuple(enabled)
        g = groups.setdefault(flags, len(groups))
        if g == len(group_pool):
            group_pool.append(flags)
        visibility_refs.append(g)
    assert len(groups) <= 256 and len(channels) < 65536 and len(color_pool) < 65536
    data = ['#pragma once\n']
    for var, value in [('VERTICES', len(base)), ('TRIANGLES', len(triangles)), ('KEYS', len(raw)),
                       ('CHANNELS', len(pool)), ('GROUPS', len(groups)), ('COLORS', len(color_pool))]:
        data.append(f'#define HONE_TRACK_{var} {value}u\n')
    fmt = lambda v: ','.join(map(str, v))
    data.append(rows('static const hone_track_vertex track_vertices[HONE_TRACK_VERTICES]',
                     (f'{{{{{fmt(p)}}},{{{fmt(c)}}}}},' for p, c in zip(base, channels))))
    data.append(rows('static const hone_track_triangle track_triangles[HONE_TRACK_TRIANGLES]',
                     (f'{{{{{fmt(t)}}},0x{rgb:04x},{g},{color}}},'
                      for t, (rgb, color), g in zip(triangles, material_refs, visibility_refs))))
    data.append(rows('static const int8_t track_delta[HONE_TRACK_CHANNELS][HONE_TRACK_KEYS]',
                     ('{' + fmt(seq) + '},' for _, seq in pool)))
    data.append(rows('static const uint16_t track_scale[HONE_TRACK_CHANNELS]', (fmt(s for s, _ in pool),)))
    data.append(rows('static const uint16_t track_color[HONE_TRACK_COLORS][HONE_TRACK_KEYS]',
                     ('{' + fmt(seq) + '},' for seq in color_pool)))
    bits = np.packbits(np.asarray(group_pool).T, axis=-1, bitorder='little')
    data.append(rows('static const uint8_t track_visible[HONE_TRACK_KEYS][(HONE_TRACK_GROUPS+7)/8]',
                     ('{' + fmt(seq) + '},' for seq in bits)))
    data.append(rows('static const hone_track_pose track_poses[AY_COMPANION_MOTION_POSES]',
                     ('{' + fmt(seq) + '},' for seq in poses)))
    header = ''.join(data).encode('ascii')
    (output/'hone_track_asset.h').write_bytes(header)
    digest = hashlib.sha256(header).hexdigest()
    (output/'companion_motion_config.h').write_text(f'''#pragma once
#define AY_COMPANION_COLORED_LIGHT 1
#define AY_COMPANION_MOTION_POSES {len(poses)}u
#define AY_COMPANION_INSPECTION_POSES {len(INSPECTION)}u
#define AY_COMPANION_OVERLAY_SHA256 "{digest}"
#define AY_LIVE_MODEL_SHA256 "{source_sha}"
''', encoding='ascii')
    store_bytes = len(base) * 12 + len(triangles) * 12 + len(pool) * (len(raw) + 2) + bits.size + len(color_pool)*len(raw)*2 + len(poses)*6
    return {'vertices':len(base), 'stored_triangles':len(triangles), 'keys':len(raw), 'channels':len(pool),
            'visibility_groups':len(groups), 'color_channels':len(color_pool), 'estimated_asset_bytes':store_bytes,
            'header_sha256':digest}


def main(output):
    output.mkdir(parents=True, exist_ok=True)
    scene = bpy.context.scene
    # The original shape-key drivers remain active. The export supplies its
    # own timing and tension so the breathing shares the expression clock.
    scene.objects['Breathing'].animation_data.action = None
    # Retain a three-quarter silhouette at LCD scale while bringing the far
    # eye and corner of the mouth into view.
    sight = unit(np.asarray(VIEW_DIRECTION))
    right = unit(np.cross((0, 0, 1), sight))
    up = np.cross(sight, right)
    camera = np.eye(4)
    camera[:3, :3] = (right, up, sight)
    textures = {}
    for image in bpy.data.images:
        if image.name.startswith(('Gaze eye', 'Blink fixed')):
            width, height = image.size
            pixels = np.empty(width * height * 4, np.float32)
            image.pixels.foreach_get(pixels)
            textures[image.name] = pixels.reshape((height, width, 4))
    keys = (*INSPECTION, *(motion(frame) for frame in KEYS))
    topology = {}
    neutral = extract(scene, motion(0), camera, textures, topology)
    shadows = bake_shadow_pattern(neutral, camera)
    sampled = [neutral if key[0] == 'motion-000' else extract(scene, key, camera, textures, topology) for key in keys]
    geometry, indices, masks, palette = [], [], [], []
    offset = 0
    for name in sampled[0]:
        registered = [sample[name] for sample in sampled]
        verts, tris, show, rgb = map(np.stack, zip(*registered))
        assert np.all(tris == tris[0]), f'Topology changes over the clip: {name}'
        live = np.flatnonzero(np.any(show, axis=0))
        if not len(live):
            continue
        mapping, remap = np.unique(tris[0][live], return_inverse=True)
        if name in shadows:
            rgb = apply_shadow_pattern(rgb, shadows[name])
        geometry.append(verts[:, mapping])
        indices.append(remap.reshape((-1, 3)) + offset)
        masks.append(show[:, live])
        palette.append(rgb[:, live])
        offset += len(mapping)
    verts = np.concatenate(geometry, axis=1)
    tris = np.concatenate(indices, axis=0)
    mask = np.concatenate(masks, axis=1)
    colors = color565(np.concatenate(palette, axis=1))
    # Framing uses only visible triangles in each pose, including comic marks.
    all_visible = np.concatenate([v[np.unique(tris[m])] for v, m in zip(verts, mask)])
    low, high = np.min(all_visible, axis=0), np.max(all_visible, axis=0)
    scale = min(204 / (high[0]-low[0]), 112 / (high[1]-low[1]))
    center = .5 * (low + high)
    verts = verts - center
    verts *= (scale * 16, -scale * 16, 1500)
    verts += (WIDTH * 8, (HEIGHT + 6) * 8, 0)
    assert np.isfinite(verts).all() and np.max(np.abs(verts)) < 32767
    raw = np.rint(verts).astype(np.int16)
    poses = schedule()
    sha = hashlib.sha256(Path(bpy.data.filepath).read_bytes()).hexdigest()
    report = emit(output, raw, tris, mask, colors, poses, sha)
    report.update({'source_sha256':sha, 'inspection_names':[record[0] for record in INSPECTION],
                   'active_triangles':[int(m.sum()) for m in mask], 'neutral_triangles':int(mask[len(INSPECTION)].sum()),
                   'frames':len(poses), 'period_ms':PERIOD_MS, 'motion_frames':MOTION_FRAMES,
                   'rig_bones':len(scene.objects['Rig62 recorded reconstruction'].data.bones),
                   'shadowed_faces':sum(int(np.count_nonzero(mask)) for mask in shadows.values()),
                   'reference_bounds': [low.tolist(), high.tolist()]})
    np.savez_compressed(output/'track-preview.npz', positions=raw, triangles=tris, visible=mask, colors=colors, poses=poses)
    (output/'asset-report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
    print(json.dumps(report))


if __name__ == '__main__':
    main(Path(sys.argv[sys.argv.index('--') + 1]).resolve())
