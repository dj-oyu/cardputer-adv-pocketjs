"""Export an authored Blender character as a Hone character asset (.hca).

blender -b cloud.blend --disable-autoexec --python-exit-code 1 \
  --python export_character_track.py -- cloud OUTPUT_DIRECTORY
(or `python export_character_track.py cloud OUTPUT_DIRECTORY` with the bpy wheel)

The asset keeps the track-model contract of export_companion_track.py:
fixed topology, signed per-key deltas, RGB565 colors, a pose schedule.  Two
things differ.  Meshes are reduced once on the neutral pose and every other
key moves the reduced vertices by the displacement of the nearest source
vertices, so topology cannot change over the clip.  Faces and comic marks are
not baked into per-key visibility; each group carries a preset mask and an
emote requirement that the player evaluates, so any face works with any clip.
"""

import hashlib
import json
import struct
import sys
from pathlib import Path

import bpy
import numpy as np

WIDTH, HEIGHT = 240, 135
MAGIC = b"HCA1"
LABEL_BYTES = 12

CLOUD = {
    "name": "cloud",
    "face_controls": "Cloud face controls",
    "face_property": "cloud_face_preset",
    "emote_controls": "Cloud emote controls",
    "presets": ["NORMAL", "HAPPY", "WORRIED", "SURPRISED", "SLEEPY", "GAG"],
    "emotes": ["sweat", "confusion", "anger"],
    "camera": "Primary 15 degrees",
    "skip": ("Stage",),
    "key_step": 4,
    "period_ms": 42,
    "light": (-.40, .55, .75),
}

BODY_BANDS = [tuple((1 - i / 6) * u + (i / 6) * v for u, v in zip((.99, .983, 1.0), (.52, .71, 1.0)))
              for i in range(7)]
RAINBOW = [(.85, .71, .98), (.72, .85, .99), (.70, .92, .96), (.98, .81, .88), (.99, .89, .79)]


def cap(name):
    """Triangle budget per source object (the practical-trim 1,775 budget)."""
    if name == "Cloud body":
        return 640
    if name.startswith("Floating hand"):
        return 72
    if name.startswith("Halo vapor"):
        return 88
    if name.startswith("Eye rim"):
        return 36
    if name.startswith("Eye white"):
        return 40
    if name.startswith("Pupil"):
        return 44
    if name.startswith("Catchlight small"):
        return 12
    if name.startswith("Catchlight"):
        return 24
    if name.startswith("Cheek"):
        return 32
    if name.startswith("Brow"):
        return 50
    if name.startswith("Open smile"):
        return 77
    return 20


def unit(values):
    return values / np.maximum(np.linalg.norm(values, axis=-1, keepdims=True), 1e-8)


def linear_to_srgb(rgb):
    rgb = np.asarray(rgb, dtype=np.float64)
    return np.where(rgb < .00313, rgb * 12.92, 1.055 * np.maximum(rgb, 0) ** (1 / 2.4) - .055)


def color565(colors):
    quant = np.floor(np.clip(colors, 0, 1) * (31, 63, 31) + .5).astype(np.uint16)
    return (quant[..., 0] << 11) | (quant[..., 1] << 5) | quant[..., 2]


class Rig:
    def __init__(self, config):
        self.config = config
        self.scene = bpy.context.scene
        self.face = self.scene.objects[config["face_controls"]]
        self.emote = self.scene.objects[config["emote_controls"]]
        self.objects = [o for o in self.scene.objects if o.type in ("MESH", "CURVE") and o.name not in config["skip"]]

    def pose(self, frame, preset, emote):
        self.scene.frame_set(frame)
        self.face[self.config["face_property"]] = float(preset)
        self.face.update_tag()
        for name in self.config["emotes"]:
            self.emote[name] = 1.0 if name == emote else 0.0
        self.emote.update_tag()
        bpy.context.view_layer.update()

    def evaluate(self, wanted):
        """world positions and visibility for the wanted objects in the current pose"""
        dg = bpy.context.evaluated_depsgraph_get()
        out = {}
        for obj in wanted:
            ev = obj.evaluated_get(dg)
            visible = (not ev.hide_render) and max(abs(v) for v in ev.scale) >= 1e-4
            mesh = ev.to_mesh()
            try:
                mesh.calc_loop_triangles()
                world = np.array([ev.matrix_world @ v.co for v in mesh.vertices], dtype=np.float64)
                polys = [tuple(p.vertices) for p in mesh.polygons]
                tris = np.array([t.vertices for t in mesh.loop_triangles], dtype=np.int64).reshape(-1, 3)
                color = tuple(mesh.materials[0].diffuse_color[:3]) if len(mesh.materials) and mesh.materials[0] else (.5, .5, .5)
            finally:
                ev.to_mesh_clear()
            out[obj.name] = (world, polys, tris, color, visible)
        return out


def reduce_mesh(world, polys, budget, scene):
    """Collapse-decimate once on the reference pose. Returns (vertices, triangles)."""
    mesh = bpy.data.meshes.new("reduce")
    mesh.from_pydata(world.tolist(), [], [list(p) for p in polys])
    mesh.update()
    obj = bpy.data.objects.new("reduce", mesh)
    scene.collection.objects.link(obj)
    try:
        count = sum(max(len(p) - 2, 1) for p in polys)
        if count > budget:
            mod = obj.modifiers.new("d", "DECIMATE")
            mod.ratio = budget / count
            mod.use_collapse_triangulate = True
        bpy.context.view_layer.update()
        ev = obj.evaluated_get(bpy.context.evaluated_depsgraph_get())
        out = ev.to_mesh()
        try:
            out.calc_loop_triangles()
            verts = np.array([v.co for v in out.vertices], dtype=np.float64)
            tris = np.array([t.vertices for t in out.loop_triangles], dtype=np.int64).reshape(-1, 3)
        finally:
            ev.to_mesh_clear()
    finally:
        scene.collection.objects.unlink(obj)
        bpy.data.objects.remove(obj)
        bpy.data.meshes.remove(mesh)
    return verts, tris


def transfer_weights(reduced, source, k=4):
    """inverse-distance weights over the k nearest source vertices"""
    index = np.zeros((len(reduced), min(k, len(source))), dtype=np.int64)
    weight = np.zeros(index.shape)
    for start in range(0, len(reduced), 256):
        chunk = reduced[start:start + 256]
        d = np.linalg.norm(chunk[:, None, :] - source[None, :, :], axis=-1)
        near = np.argsort(d, axis=1)[:, :index.shape[1]]
        dist = np.take_along_axis(d, near, axis=1)
        w = 1.0 / (dist + 1e-6)
        index[start:start + 256] = near
        weight[start:start + 256] = w / w.sum(axis=1, keepdims=True)
    return index, weight


def vertex_shade_normals(verts, tris):
    normals = np.zeros_like(verts)
    a, b, c = verts[tris].transpose((1, 0, 2))
    face = np.cross(b - a, c - a)
    for i in range(3):
        np.add.at(normals, tris[:, i], face)
    return unit(normals[tris].mean(axis=1))


def shade(albedo, normal_cam, light):
    key = np.clip(normal_cam @ unit(np.asarray(light)), 0, 1)
    fill = np.clip(normal_cam @ unit(np.array([.55, -.35, .75])), 0, 1)
    base = linear_to_srgb(albedo)
    body = (.74 + .20 * key + .06 * fill)[..., None]
    cool = np.array([.62, .72, 1.0])
    shadow = (.28 * (1 - key))[..., None]
    shaded = base * body * (1 - shadow) + base * cool * shadow
    return np.clip(shaded, 0, 1)


def rasterize(pos, tris, colors, visible, size=(WIDTH, HEIGHT)):
    """small reference rasterizer used only for the preview sheet"""
    w, h = size
    image = np.full((h, w, 3), 24, np.uint8)
    depth = np.full((h, w), -1e30)
    order = np.arange(len(tris))
    for i in order:
        if not visible[i]:
            continue
        a, b, c = pos[tris[i]] / (16.0, 16.0, 1.0)
        area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
        if abs(area) < 1e-9:
            continue
        x0, x1 = int(max(min(a[0], b[0], c[0]), 0)), int(min(max(a[0], b[0], c[0]) + 1, w))
        y0, y1 = int(max(min(a[1], b[1], c[1]), 0)), int(min(max(a[1], b[1], c[1]) + 1, h))
        if x0 >= x1 or y0 >= y1:
            continue
        xs, ys = np.meshgrid(np.arange(x0, x1) + .5, np.arange(y0, y1) + .5)
        w0 = ((b[0] - xs) * (c[1] - ys) - (b[1] - ys) * (c[0] - xs)) / area
        w1 = ((c[0] - xs) * (a[1] - ys) - (c[1] - ys) * (a[0] - xs)) / area
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        z = w0 * a[2] + w1 * b[2] + w2 * c[2]
        view = depth[y0:y1, x0:x1]
        win = inside & (z > view)
        view[win] = z[win]
        r5, g6, b5 = colors[i] >> 11, (colors[i] >> 5) & 63, colors[i] & 31
        image[y0:y1, x0:x1][win] = (r5 * 255 // 31, g6 * 255 // 63, b5 * 255 // 31)
    return image


def export(config, output, sheet=True):
    output.mkdir(parents=True, exist_ok=True)
    rig = Rig(config)
    scene = rig.scene
    states = [(0, None), (1, None), (2, None), (3, None), (4, None), (5, None),
              (0, config["emotes"][0]), (0, config["emotes"][1]), (0, config["emotes"][2])]

    # 1. which pose shows each source object?
    ref_frame = scene.frame_start
    visible_in = {}
    reference = {}
    for state in states:
        rig.pose(ref_frame, *state)
        for name, data in rig.evaluate(rig.objects).items():
            visible_in.setdefault(name, {})[state] = data[4]
            if data[4] and name not in reference:
                reference[name] = (state, data)
    live = [o for o in rig.objects if o.name in reference]
    print("live objects", len(live), "of", len(rig.objects))

    # 2. reduce each object once; derive transfer weights from the reference pose
    reduced = {}
    for obj in live:
        state, (world, polys, tris, color, _) = reference[obj.name]
        verts, rtris = reduce_mesh(world, polys, cap(obj.name), scene)
        index, weight = transfer_weights(verts, world)
        reduced[obj.name] = {"state": state, "rest": world, "verts": verts, "tris": rtris,
                             "index": index, "weight": weight, "color": color}
    print("reduced triangles", sum(len(r["tris"]) for r in reduced.values()))

    # 3. sample keys
    start, end = scene.frame_start, scene.frame_end
    key_frames = list(range(start, end + 1, config["key_step"]))
    if key_frames[-1] != end:
        key_frames.append(end)
    by_state = {}
    for name, r in reduced.items():
        by_state.setdefault(r["state"], []).append(name)
    objects_by_name = {o.name: o for o in live}
    samples = []  # key -> name -> (verts world)
    for frame in key_frames:
        row = {}
        for state, names in by_state.items():
            rig.pose(frame, *state)
            got = rig.evaluate([objects_by_name[n] for n in names])
            for n in names:
                r = reduced[n]
                displacement = got[n][0] - r["rest"]
                row[n] = r["verts"] + np.einsum("vk,vkc->vc", r["weight"], displacement[r["index"]])
        samples.append(row)
        print("key", frame, flush=True)

    # 4. groups, triangles, camera
    names = [o.name for o in live]
    preset_count = len(config["presets"])
    groups = {}
    group_of = {}
    for name in names:
        preset_mask = 0
        emote_req = 0
        regular = any(visible_in[name].get((p, None)) for p in range(preset_count))
        if regular:
            for p in range(preset_count):
                if visible_in[name].get((p, None)):
                    preset_mask |= 1 << p
        else:
            for e, emote in enumerate(config["emotes"]):
                if visible_in[name].get((0, emote)):
                    emote_req |= 1 << e
            preset_mask = (1 << preset_count) - 1
        group_of[name] = groups.setdefault((preset_mask, emote_req), len(groups))
    assert len(groups) <= 256

    cam = scene.objects[config["camera"]]
    world_to_camera = np.array(cam.matrix_world.inverted())
    rot, off = world_to_camera[:3, :3], world_to_camera[:3, 3]

    def to_camera(points):
        return points @ rot.T + off

    all_pts = []
    vertex_base = {}
    total = 0
    for name in names:
        vertex_base[name] = total
        total += len(reduced[name]["verts"])
    for row in samples:
        for name in names:
            all_pts.append(to_camera(row[name]))
    cam_all = np.concatenate(all_pts)
    low, high = cam_all.min(axis=0), cam_all.max(axis=0)
    scale = min(214 / (high[0] - low[0]), 120 / (high[1] - low[1]))
    centre = .5 * (low + high)
    zscale = 14000 / max(high[2] - low[2], 1e-6)

    def to_screen(points):
        p = to_camera(points) - centre
        return np.stack([p[:, 0] * scale * 16 + WIDTH * 8, -p[:, 1] * scale * 16 + HEIGHT * 8 + 8,
                         p[:, 2] * zscale], axis=1)

    keys = len(key_frames)
    positions = np.zeros((keys, total, 3))
    tri_list, tri_group, tri_albedo, tri_object = [], [], [], []
    for name in names:
        r = reduced[name]
        base = vertex_base[name]
        for k in range(keys):
            positions[k, base:base + len(r["verts"])] = to_screen(samples[k][name])
        tri_list.append(r["tris"] + base)
        tri_group.extend([group_of[name]] * len(r["tris"]))
        tri_object.extend([name] * len(r["tris"]))
        if name == "Cloud body" or name.startswith("Floating hand"):
            z = r["verts"][r["tris"]][:, :, 2].mean(axis=1)
            t = (z - z.min()) / max(z.max() - z.min(), 1e-6)
            albedo = [BODY_BANDS[max(0, min(6, round((1 - v) * 5.45)))] for v in t]
        elif name.startswith("Halo vapor"):
            albedo = [RAINBOW[int(name.split()[-1]) % len(RAINBOW)]] * len(r["tris"])
        else:
            albedo = [r["color"]] * len(r["tris"])
        tri_albedo.extend(albedo)
    tris = np.concatenate(tri_list)
    albedo = np.asarray(tri_albedo, dtype=np.float64)
    group = np.asarray(tri_group)

    # 5. per-key colors from camera-space smooth normals
    colors = np.zeros((keys, len(tris)), dtype=np.uint16)
    for k in range(keys):
        cam_pos = np.zeros((total, 3))
        for name in names:
            r = reduced[name]
            cam_pos[vertex_base[name]:vertex_base[name] + len(r["verts"])] = to_camera(samples[k][name])
        normal = np.zeros((len(tris), 3))
        for name in names:
            sel = np.flatnonzero(np.asarray(tri_object) == name)
            local = tris[sel] - vertex_base[name]
            normal[sel] = vertex_shade_normals(cam_pos[vertex_base[name]:vertex_base[name] + len(reduced[name]["verts"])], local)
        flip = normal[:, 2] < 0  # two-sided surfaces keep a lit side toward the viewer
        normal[flip] *= -1
        colors[k] = color565(shade(albedo, normal, config["light"]))

    # 6. channel encoding (identical scheme to export_companion_track.emit)
    raw = np.rint(positions).astype(np.int32)
    assert np.abs(raw[:, :, :2]).max() < 8000 and np.abs(raw[:, :, 2]).max() < 32767
    base_key = raw[0]
    displacement = raw - base_key
    steps = np.maximum(1, (np.max(np.abs(displacement), axis=0) + 126) // 127)
    delta = np.rint(displacement / steps).astype(np.int8)
    pool = [(1, (0,) * keys)]
    channel_map = {pool[0]: 0}
    channels = np.zeros(base_key.shape, dtype=np.int64)
    for v in range(total):
        for axis in range(3):
            seq = tuple(int(x) for x in delta[:, v, axis])
            if any(seq):
                item = (int(steps[v, axis]), seq)
                channels[v, axis] = channel_map.setdefault(item, len(channel_map))
                if channels[v, axis] == len(pool):
                    pool.append(item)
    color_pool = [(0,) * keys]
    color_map = {}
    color_ref = []
    for i in range(len(tris)):
        seq = tuple(int(x) for x in colors[:, i])
        c = 0
        if len(set(seq)) > 1:
            c = color_map.setdefault(seq, len(color_map) + 1)
            if c == len(color_pool):
                color_pool.append(seq)
        color_ref.append((seq[0], c))
    assert len(channel_map) < 65536 and len(color_pool) < 65536

    # 7. poses: one per frame of the clip, blending between neighbouring keys
    span = end - start
    poses = []
    for f in range(span):
        frame = start + f
        idx = max(i for i, kf in enumerate(key_frames) if kf <= frame)
        idx = min(idx, keys - 2)
        a, b = key_frames[idx], key_frames[idx + 1]
        poses.append((idx, idx + 1, int(round(255 * (frame - a) / (b - a)))))
    clips = [("idle", 0, len(poses), config["period_ms"])]

    # 8. serialise
    vert_bytes = b"".join(struct.pack("<3h3H", *base_key[v].tolist(), *channels[v].tolist()) for v in range(total))
    tri_bytes = b"".join(struct.pack("<3HHBxH", *map(int, tris[i]), color_ref[i][0], int(group[i]), color_ref[i][1])
                         for i in range(len(tris)))
    delta_bytes = b"".join(struct.pack(f"<{keys}b", *seq) for _, seq in pool)
    scale_bytes = struct.pack(f"<{len(pool)}H", *(s for s, _ in pool))
    color_bytes = b"".join(struct.pack(f"<{keys}H", *seq) for seq in color_pool)
    group_bytes = b"".join(struct.pack("<HH", m, e) for (m, e), _ in sorted(groups.items(), key=lambda kv: kv[1]))
    pose_bytes = b"".join(struct.pack("<HHBx", a, b, bl) for a, b, bl in poses)
    clip_bytes = b"".join(struct.pack("<16sHHHH", n.encode(), s, c, p, 0) for n, s, c, p in clips)
    labels = [*config["presets"], *[e.upper() for e in config["emotes"]]]
    label_bytes = b"".join(l.encode()[:LABEL_BYTES - 1].ljust(LABEL_BYTES, b"\0") for l in labels)
    tables = [vert_bytes, tri_bytes, delta_bytes, scale_bytes, color_bytes, group_bytes, pose_bytes, clip_bytes, label_bytes]
    header_size = 128
    offsets, cursor = [], header_size
    for t in tables:
        cursor = (cursor + 3) & ~3
        offsets.append(cursor)
        cursor += len(t)
    source_sha = hashlib.sha256(Path(bpy.data.filepath).read_bytes()).digest()
    header = struct.pack("<4sHH9I9I32s16s", MAGIC, 1, 0,
                         total, len(tris), keys, len(pool), len(groups), len(color_pool), len(poses), len(clips),
                         preset_count, *offsets, source_sha, config["name"].encode())
    header = header.ljust(header_size, b"\0")
    blob = bytearray(header)
    for t, off in zip(tables, offsets):
        blob.extend(b"\0" * (off - len(blob)))
        blob.extend(t)
    path = output / f"{config['name']}.hca"
    path.write_bytes(bytes(blob))

    report = {"name": config["name"], "bytes": len(blob), "vertices": total, "triangles": len(tris), "keys": keys,
              "channels": len(pool), "groups": len(groups), "color_channels": len(color_pool), "poses": len(poses),
              "frames": span, "period_ms": config["period_ms"], "presets": config["presets"], "emotes": config["emotes"],
              "triangles_per_object": {n: len(reduced[n]["tris"]) for n in names},
              "source_sha256": source_sha.hex(), "hca_sha256": hashlib.sha256(bytes(blob)).hexdigest(),
              "key_frames": key_frames}

    # 9. preview sheet from the exported tables themselves
    if sheet:
        from PIL import Image
        tiles = []
        shown = [(0, 0, None), (0, 1, None), (0, 2, None), (0, 3, None), (0, 4, None), (0, 5, None),
                 (0, 0, "sweat"), (0, 0, "confusion"), (0, 0, "anger"), (10, 0, None), (20, 1, None), (30, 5, "anger")]
        for key, preset, emote in shown:
            pos = raw[key].astype(np.float64)
            group_list = [k for k, _ in sorted(groups.items(), key=lambda kv: kv[1])]
            vis = np.zeros(len(tris), bool)
            for i in range(len(tris)):
                m, e = group_list[group[i]]
                vis[i] = bool(m & (1 << preset)) and (e == 0 or bool(emote and e & (1 << config["emotes"].index(emote))))
            tiles.append(Image.fromarray(rasterize(pos, tris, colors[key], vis)))
        cols = 4
        sheet_img = Image.new("RGB", (cols * WIDTH, ((len(tiles) + cols - 1) // cols) * HEIGHT))
        for i, t in enumerate(tiles):
            sheet_img.paste(t, ((i % cols) * WIDTH, (i // cols) * HEIGHT))
        sheet_img.save(output / "preview.png")
    (output / "asset-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k not in ("triangles_per_object", "key_frames")}))


if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    blend = None
    if len(argv) == 3:
        blend, argv = argv[0], argv[1:]
        bpy.ops.wm.open_mainfile(filepath=blend)
    if argv[0] != "cloud":
        raise SystemExit("unknown character " + argv[0])
    export(CLOUD, Path(argv[1]).resolve())
