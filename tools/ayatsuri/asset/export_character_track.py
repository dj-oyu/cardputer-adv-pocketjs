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
import os
import struct
import sys
from pathlib import Path

import bpy
import numpy as np

WIDTH, HEIGHT = 240, 135
MAGIC = b"HCA1"
VERSION = 2
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
    # name, source blend relative to the main one (None = same file), frame range, period
    # name, other blend (None = the main one), unused, period ms, frames per key
    "clips": [("idle", None, None, 42, 8),
              ("pop", "../pop-chain-01/cloud-pop-chain.blend", None, 42, 4)],
    "period_ms": 42,
    "light": (-.40, .55, .75),
}

def cloud_apply(rig, preset, emote):
    face = rig.scene.objects.get(rig.config["face_controls"])
    marks = rig.scene.objects.get(rig.config["emote_controls"])
    if face is not None:
        face[rig.config["face_property"]] = float(preset)
        face.update_tag()
    if marks is not None:
        for name in rig.config["emotes"]:
            marks[name] = 1.0 if name == emote else 0.0
        marks.update_tag()


CLOUD["select"] = lambda o: o.type in ("MESH", "CURVE") and o.name not in ("Stage",)
CLOUD["cap"] = lambda name: None if name.startswith("Cloud emote") else cap(name)
CLOUD["apply"] = cloud_apply
CLOUD["albedo"] = "object"
CLOUD["layered"] = ("Cloud emote",)


# Human: the figure is rigged and already low-poly, so nothing is decimated. The
# face is a texture on a 160-triangle head; its art is baked into subdivided
# triangles with one color per face preset. Set pieces (the engine bay) are left out.
HUMAN_FACES = [("NORMAL", 0, 0, 0), ("WORRIED", 1, 0, 0), ("SURPRISED", 2, 0, 0), ("GAG", 3, 0, 0),
               ("SLEEPY", 4, 0, 0), ("SMILE", 0, 0, 1), ("BLINK", 0, 1, 0)]


def human_select(o):
    if o.type not in ("MESH", "CURVE"):
        return False
    if any(m.type == "ARMATURE" for m in o.modifiers):
        return True
    return o.name.startswith(("Comic emote", "Engine chair", "Chair • stationary"))


def human_apply(rig, preset, emote):
    arm = rig.scene.objects.get("Provisional anatomical joints")
    if arm is None:
        return
    _, expression, blink, smile = HUMAN_FACES[preset]
    arm["Face Expression"] = expression
    arm["Face Blink"] = blink
    arm["Face Smile"] = smile
    arm["Comic Emote"] = rig.config["emotes"].index(emote) + 1 if emote else 0
    arm.update_tag()


HUMAN = {
    "name": "human",
    "presets": [f[0] for f in HUMAN_FACES],
    "emotes": ["sweat", "confusion", "anger"],
    "camera": None,
    "select": human_select,
    "cap": lambda name: None,
    "apply": human_apply,
    "albedo": "material",
    "light": (-.40, .55, .75),
    "quant": tuple(int(x) for x in os.environ.get("HUMAN_QUANT", "4,64").split(",")),
    "color_tol": (3, 6, 3),
    "view": (-.31, -.95, .16),
    # The comic marks were composed for another camera and sit far from the head. Stand each one
    # up facing this camera at the head's upper right, following the head through every key.
    "billboard": ("Comic emote", "Sculpted cheeks, jaw and cranium", (1.30, 0.75)),
    "face_art": {
        "object": "Sculpted cheeks, jaw and cranium",
        "uv": "FaceGraphic_Ortho_512",
        "weight": "FaceGraphic_front_weight",
        "skin": (.95, .80, .70),
        "images": {"NORMAL": "Packed face neutral larger eyes", "WORRIED": "Packed face expression worried",
                   "SURPRISED": "Packed face expression surprised", "GAG": "Packed Gekiga GPT Image face with original inner crease",
                   "SLEEPY": "Packed face expression sleepy", "SMILE": "Packed face smile", "BLINK": "Packed face blink"},
    },
    # name, file under the archive root, unused, period ms, frames per key
    "clips": [],
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
    def __init__(self, config, controls=True):
        self.config = config
        self.scene = bpy.context.scene
        self.controls = controls
        self.objects = [o for o in self.scene.objects if config["select"](o)]

    def pose(self, frame, preset, emote):
        self.scene.frame_set(frame)
        if self.controls:
            self.config["apply"](self, preset, emote)
        bpy.context.view_layer.update()

    def evaluate(self, wanted):
        """world positions, topology and materials of the wanted objects in the current pose"""
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
                matidx = np.array([t.material_index for t in mesh.loop_triangles], dtype=np.int64)
                matcolors = [tuple(m.diffuse_color[:3]) if m else (.5, .5, .5) for m in mesh.materials] or [(.5, .5, .5)]
                color = matcolors[0]
            finally:
                ev.to_mesh_clear()
            out[obj.name] = (world, polys, tris, color, visible, matidx, matcolors)
        return out


def resolve_clip(config, main, relative):
    """a clip file: relative to the root (or the main file), or 'archive.zip::inner.blend'"""
    root = Path(config.get("root") or main.parent)
    if "::" in relative:
        import tempfile
        import zipfile
        archive, inner = relative.split("::", 1)
        target = Path(tempfile.gettempdir()) / "hca-clips" / Path(archive).stem
        target.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(root / archive) as z:
            z.extract(inner, target)
        return (target / inner).resolve()
    return (root / relative).resolve()


def names_all(reduced):
    return list(reduced)


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
    # Light terms are held to 1/14 steps: a triangle whose lit value barely moves
    # then keeps one color for several keys, which lets color tracks be shared.
    key = np.round(np.clip(normal_cam @ unit(np.asarray(light)), 0, 1) * 14) / 14
    fill = np.round(np.clip(normal_cam @ unit(np.array([.55, -.35, .75])), 0, 1) * 14) / 14
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


def srgb_to_linear(rgb):
    rgb = np.asarray(rgb, dtype=np.float64)
    return np.where(rgb <= .04045, rgb / 12.92, ((rgb + .055) / 1.055) ** 2.4)


def lattice(n):
    """barycentric points and triangles of an n x n subdivision"""
    points = [(a, b) for a in range(n + 1) for b in range(n + 1 - a)]
    index = {p: i for i, p in enumerate(points)}
    tris = []
    for a in range(n):
        for b in range(n - a):
            tris.append((index[(a, b)], index[(a + 1, b)], index[(a, b + 1)]))
            if a + b < n - 1:
                tris.append((index[(a + 1, b)], index[(a + 1, b + 1)], index[(a, b + 1)]))
    return points, tris


def face_art(config, obj, r):
    """Replace the head's reduced mesh by one whose front triangles are subdivided where the
    face art has detail, with one sampled color per face preset on those triangles."""
    spec = config["face_art"]
    names = config["presets"]
    dg = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(dg)
    mesh = ev.to_mesh()
    try:
        mesh.calc_loop_triangles()
        layer = mesh.uv_layers[spec["uv"]].data
        tri_uv = np.array([[layer[loop].uv for loop in t.loops] for t in mesh.loop_triangles], dtype=np.float64)
        weights = np.array([d.value for d in mesh.attributes[spec["weight"]].data], dtype=np.float64)
    finally:
        ev.to_mesh_clear()
    tris = r["tris"]
    front = weights[tris].mean(axis=1) > .5

    def pixels(name):
        im = bpy.data.images[name]
        w, h = im.size
        return np.array(im.pixels[:], dtype=np.float32).reshape(h, w, 4)

    images = {n: pixels(spec["images"][n]) for n in names}
    h, w = images[names[0]].shape[:2]
    skin = np.asarray(spec["skin"], dtype=np.float32)

    def box(img, pad=24):
        ys, xs = np.nonzero(img[..., 3] > .05)
        mask = np.zeros(img.shape[:2], bool)
        mask[max(ys.min() - pad, 0):ys.max() + pad, max(xs.min() - pad, 0):xs.max() + pad] = True
        return mask

    def over(base, layer_img):
        a = layer_img[..., 3:4]
        return base * (1 - a) + layer_img[..., :3] * a

    neutral = images["NORMAL"]
    comp = {}
    for name in names:
        base = np.broadcast_to(skin, (h, w, 3)).copy()
        if name in ("SMILE", "BLINK"):
            erased = neutral.copy()
            erased[box(images[name]), 3] = 0
            base = over(base, erased)
        comp[name] = over(base, images[name])
    art = np.max([im[..., 3] for im in images.values()], axis=0) > .08

    def at(uv):
        x = np.clip((uv[..., 0] * w).astype(int), 0, w - 1)
        y = np.clip((uv[..., 1] * h).astype(int), 0, h - 1)
        return x, y

    grid = [(i / 6, j / 6) for i in range(7) for j in range(7 - i)]
    new_verts, new_index, new_weight, new_tris, new_mat, new_art = [], [], [], [], [], {}
    for t in range(len(tris)):
        src = tris[t]
        uv = tri_uv[t]
        n = 1
        if front[t]:
            pts = np.array([(1 - a - b) * uv[0] + a * uv[1] + b * uv[2] for a, b in grid])
            x, y = at(pts)
            coverage = art[y, x].mean()
            n = 1 if coverage < .03 else 2 if coverage < .3 else 3
        points, sub = lattice(n)
        base = len(new_verts)
        corner = r["verts"][src]
        for a, b in points:
            bary = np.array([1 - (a + b) / n, a / n, b / n])
            new_verts.append(bary @ corner)
            new_index.append(src)
            new_weight.append(bary)
        for tri in sub:
            index = len(new_tris)
            new_tris.append([base + i for i in tri])
            new_mat.append(0)
            if n > 1:
                centre = np.mean([points[i] for i in tri], axis=0)
                samples = []
                for a, b in [centre, *[(centre + (np.array(points[i]) - centre) * .5) for i in tri]]:
                    q = ((1 - (a + b) / n) * uv[0] + a / n * uv[1] + b / n * uv[2])[None]
                    x, y = at(q)
                    samples.append([comp[name][y[0], x[0]] for name in names])
                new_art[index] = np.mean(samples, axis=0)  # (faces, 3) sRGB
    r["verts"] = np.asarray(new_verts)
    r["tris"] = np.asarray(new_tris, dtype=np.int64)
    r["index"] = np.asarray(new_index, dtype=np.int64)
    r["weight"] = np.asarray(new_weight)
    r["matidx"] = np.asarray(new_mat, dtype=np.int64)
    r["matcolors"] = [tuple(srgb_to_linear(skin))]
    r["art"] = new_art
    print("face art: head triangles", len(tris), "->", len(new_tris), "with presets on", len(new_art))


def export(config, output, sheet=True):
    output.mkdir(parents=True, exist_ok=True)
    rig = Rig(config)
    scene = rig.scene
    states = [(p, None) for p in range(len(config["presets"]))] + [(0, e) for e in config["emotes"]]

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
        state, (world, polys, tris, color, _, matidx, matcolors) = reference[obj.name]
        budget = config["cap"](obj.name)
        if budget is None:
            # Already low-poly and skinned: keep every vertex, so the transfer is exact.
            verts, rtris = world, tris
            index, weight = np.arange(len(world))[:, None], np.ones((len(world), 1))
            rmat = matidx
        else:
            verts, rtris = reduce_mesh(world, polys, budget, scene)
            index, weight = transfer_weights(verts, world)
            rmat = np.zeros(len(rtris), dtype=np.int64)
        reduced[obj.name] = {"state": state, "rest": world, "verts": verts, "tris": rtris, "index": index,
                             "weight": weight, "color": color, "matidx": rmat, "matcolors": matcolors, "art": {}}
        art = config.get("face_art")
        if art and obj.name == art["object"]:
            face_art(config, obj, reduced[obj.name])
    print("reduced triangles", sum(len(r["tris"]) for r in reduced.values()))

    # 3. sample keys, clip by clip. Keys are shared by index; a pose blends two keys of one clip.
    source_path = Path(bpy.data.filepath)
    if config.get("view"):
        # A fixed three-quarter view: sight points from the subject toward the viewer.
        sight = unit(np.asarray(config["view"], dtype=np.float64))
        right = unit(np.cross((0, 0, 1), sight))
        up = np.cross(sight, right)
        w2c = np.eye(4)
        w2c[:3, :3] = (right, up, sight)
        camera_matrix = np.linalg.inv(w2c)
    else:
        camera_object = scene.objects[config["camera"]] if config["camera"] else scene.camera
        camera_matrix = np.array(camera_object.matrix_world)
    by_state = {}
    for name, r in reduced.items():
        by_state.setdefault(r["state"], []).append(name)
    objects_by_name = {o.name: o for o in live}

    def clip_frames(first, last, step):
        frames = list(range(first, last + 1, step))
        if frames[-1] != last:
            frames.append(last)
        return frames

    samples = []      # key -> name -> reduced world vertices
    key_clip = []     # key -> (clip index, frame)
    clip_keys = []    # clip -> [key indices]
    clip_span = []    # clip -> number of frames in its loop
    frames = clip_frames(scene.frame_start, scene.frame_end, config["clips"][0][4])
    clip_keys.append([])
    clip_span.append(scene.frame_end - scene.frame_start)
    for frame in frames:
        row = {}
        for state, names in by_state.items():
            rig.pose(frame, *state)
            got = rig.evaluate([objects_by_name[n] for n in names])
            for n in names:
                r = reduced[n]
                displacement = got[n][0] - r["rest"]
                row[n] = r["verts"] + np.einsum("vk,vkc->vc", r["weight"], displacement[r["index"]])
        clip_keys[0].append(len(samples))
        key_clip.append((0, frame))
        samples.append(row)
        print("key", frame, flush=True)

    # Further clips come from other files that share the rig and the geometry
    # but not every object (a pop clip has no alternative faces or emotes).
    # Objects that are missing ride the nearest vertices that are present.
    for ci, (cname, relative, _, _, step) in enumerate(config["clips"][1:], start=1):
        turntable = relative.startswith("turntable::")
        bpy.ops.wm.open_mainfile(filepath=str(resolve_clip(config, source_path, relative.removeprefix("turntable::"))))
        other = Rig(config)
        scene2 = other.scene
        present = {o.name: o for o in other.objects}
        usable = {}
        for name, obj in present.items():
            if name in reduced and len(other.evaluate([obj])[name][0]) == len(reduced[name]["rest"]):
                usable[name] = obj
        # Each object is read in the pose where it is shown (an emote mark only with its emote on).
        groups = {}
        for n in usable:
            groups.setdefault(reduced[n]["state"], []).append(n)

        def sample_frame(frame):
            got = {}
            for state, ns in groups.items():
                other.pose(frame, *state)
                got.update(other.evaluate([usable[n] for n in ns]))
            return got

        missing = [n for n in names_all(reduced) if n not in usable]
        print("clip", cname, "objects present", len(usable), "borrowed", len(missing))
        rest_cloud = np.concatenate([reduced[n]["rest"] for n in usable])
        borrow = {n: transfer_weights(reduced[n]["verts"], rest_cloud) for n in missing}
        first_frame, last_frame = (1, 276) if turntable else (scene2.frame_start, scene2.frame_end)
        frames = clip_frames(first_frame, last_frame, step)
        clip_keys.append([])
        clip_span.append(last_frame - first_frame)
        if turntable:
            # The seated pose of frame 84 turns once about the vertical axis through the chair
            # post, on a short ease in and out; the stationary base does not move.
            seated = sample_frame(84)
            pivot = np.array([0.0, 0.0, 0.12])
        for frame in frames:
            if turntable:
                q = min(1.0, max(0.0, (frame - 1 - 54) / (246 - 54)))
                yaw = 2 * np.pi * (.80 * q + .20 * (q * q * (3 - 2 * q)))
                c, sn = np.cos(yaw), np.sin(yaw)
                turn = np.array([[c, -sn, 0], [sn, c, 0], [0, 0, 1]])
                got = {n: ((seated[n][0] if n.startswith("Chair • stationary")
                            else (seated[n][0] - pivot) @ turn.T + pivot), None, None, None, True)
                       for n in usable}
            else:
                got = sample_frame(frame)
            cloud = np.concatenate([got[n][0] - reduced[n]["rest"] for n in usable])
            row = {}
            for n, r in reduced.items():
                if n in usable:
                    displacement = got[n][0] - r["rest"]
                    row[n] = r["verts"] + np.einsum("vk,vkc->vc", r["weight"], displacement[r["index"]])
                    if not got[n][4]:
                        # Hidden in this clip (the chair when standing): fold it to a point
                        # so its triangles have no area and are skipped.
                        row[n] = np.tile(row[n].mean(axis=0), (len(row[n]), 1))
                else:
                    index, weight = borrow[n]
                    row[n] = r["verts"] + np.einsum("vk,vkc->vc", weight, cloud[index])
            clip_keys[ci].append(len(samples))
            key_clip.append((ci, frame))
            samples.append(row)
        print("clip", cname, "keys", len(frames), flush=True)

    # 4. groups, triangles, camera
    names = list(reduced)  # same order as `live`; the Blender objects may be gone by now
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

    world_to_camera = np.linalg.inv(camera_matrix)
    rot, off = world_to_camera[:3, :3], world_to_camera[:3, 3]

    def to_camera(points):
        return points @ rot.T + off

    billboard = config.get("billboard")
    if billboard:
        prefix, head_name, (right_offset, up_offset) = billboard
        world_up = np.array([0.0, 0.0, 1.0])
        toward_viewer = rot[2]
        for name in names:
            if not name.startswith(prefix):
                continue
            glyph = reduced[name]["verts"]
            centre = glyph.mean(axis=0)
            _, _, axes = np.linalg.svd(glyph - centre)
            # A mark is a stack of thin sheets, the ink outline behind and the colors in front. Face the
            # stack toward the viewer with the ink at the back, whichever way it was drawn.
            normal = axes[2]
            shade_of = np.array([sum(reduced[name]["matcolors"][min(int(m), len(reduced[name]["matcolors"]) - 1)])
                                 for m in reduced[name]["matidx"]])
            depth = glyph[reduced[name]["tris"]].mean(axis=1) @ normal
            ink = shade_of <= shade_of.min() + 1e-6
            if ink.any() and (~ink).any() and depth[ink].mean() > depth[~ink].mean():
                normal = -normal  # the dark outline sheet must sit behind the colored ones
            if normal @ toward_viewer < 0:
                # Looking at its back: the stack order still holds, so mirror instead of turning.
                mirror = True
            else:
                mirror = False
            up = world_up - (world_up @ normal) * normal
            up = unit(up) if np.linalg.norm(up) > 1e-6 else unit(axes[1])
            right = np.cross(up, normal)
            local = np.stack([(glyph - centre) @ right, (glyph - centre) @ up, (glyph - centre) @ normal], axis=1)
            if mirror:
                local[:, 0] *= -1  # a back view reads reversed; undo it
            # The sheets are coplanar, and equal depths go to whichever triangle comes first, so
            # give each material its own small depth step, darkest at the back.
            order = {v: i for i, v in enumerate(np.unique(shade_of.round(4)))}
            tri_rank = np.array([order[v] for v in shade_of.round(4)])
            vertex_rank = np.zeros(len(glyph))
            for corners, rank in zip(reduced[name]["tris"], tri_rank):
                vertex_rank[corners] = np.maximum(vertex_rank[corners], rank)
            local[:, 2] = vertex_rank * 0.02 * np.ptp(local[:, 1])
            # Marks were drawn for a closer camera: size each one to the head it floats beside.
            head0 = to_camera(samples[0][head_name])
            local *= 1.1 * ((head0.max(axis=0) - head0.min(axis=0))[1]) / max(np.ptp(local[:, 1]), 1e-6)
            for k, row in enumerate(samples):
                head = to_camera(row[head_name])
                head_centre = head.mean(axis=0)
                half = (head.max(axis=0) - head.min(axis=0)) / 2
                placed = head_centre + np.array([right_offset * half[0], up_offset * half[1], half[2] * 1.2]) + local
                row[name] = (placed - off) @ rot

    # Marks drawn as coplanar sheets (ink, then colors) need an explicit depth order: equal
    # depths go to the triangle that comes first, which hides the colors behind the ink.
    for name in names:
        if not name.startswith(tuple(config.get("layered", ()))) or name.startswith(
                (billboard or ("\0",))[0]):
            continue
        r = reduced[name]
        shade_of = np.array([sum(r["matcolors"][min(int(m), len(r["matcolors"]) - 1)]) for m in r["matidx"]])
        order = {v: i for i, v in enumerate(np.unique(shade_of.round(4)))}
        vertex_rank = np.zeros(len(r["verts"]))
        for corners, v in zip(r["tris"], shade_of.round(4)):
            vertex_rank[corners] = np.maximum(vertex_rank[corners], order[v])
        for row in samples:
            cam_points = to_camera(row[name])
            cam_points[:, 2] += vertex_rank * 0.02 * np.ptp(cam_points[:, :2].reshape(-1))
            row[name] = (cam_points - off) @ rot

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

    keys = len(samples)
    positions = np.zeros((keys, total, 3))
    tri_list, tri_group, tri_albedo, tri_object = [], [], [], []
    tri_offset, running = {}, 0
    for name in names:
        r = reduced[name]
        base = vertex_base[name]
        tri_offset[name] = running
        running += len(r["tris"])
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
        elif config["albedo"] == "material" or name.startswith(tuple(config.get("layered", ()))):
            albedo = [r["matcolors"][min(int(m), len(r["matcolors"]) - 1)] for m in r["matidx"]]
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
        if k == 0:
            normal0 = normal.copy()

    # Face-art triangles take their color from the face preset, lit once on the first key.
    art_tris = {tri_offset[n] + i: rgb for n in names for i, rgb in reduced[n]["art"].items()}
    preset565 = {}
    if art_tris:
        idx = np.array(sorted(art_tris))
        lin = srgb_to_linear(np.stack([art_tris[i] for i in idx]))
        faces = len(config["presets"])
        table = np.stack([color565(shade(lin[:, f], normal0[idx], config["light"])) for f in range(faces)], axis=1)
        for row, i in enumerate(idx):
            preset565[int(i)] = [int(x) for x in table[row]]
            colors[:, i] = table[row, 0]

    # Shading that never moves by more than one display step is stored once.
    r5, g6, b5 = colors >> 11, (colors >> 5) & 63, colors & 31
    tol = config.get("color_tol", (1, 2, 1))
    flat = ((r5.max(0) - r5.min(0)) <= tol[0]) & ((g6.max(0) - g6.min(0)) <= tol[1]) & ((b5.max(0) - b5.min(0)) <= tol[2])
    colors[:, flat] = colors[len(colors) // 2, flat]
    print("constant colour triangles", int(flat.sum()), "of", len(flat))

    # 6. channel encoding (identical scheme to export_companion_track.emit)
    raw = np.rint(positions).astype(np.int32)
    assert np.abs(raw[:, :, :2]).max() < 8000 and np.abs(raw[:, :, 2]).max() < 32767
    base_key = raw[0]
    displacement = raw - base_key
    # The quantum per axis is a floor on the step, so quiet vertices collapse to static
    # channels and similar motions share one. Depth only decides occlusion order.
    quantum = np.array([*config.get("quant", (1, 1))[:1] * 2, config.get("quant", (1, 1))[1]])
    steps = np.maximum(quantum, (np.max(np.abs(displacement), axis=0) + 126) // 127)
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
    preset_rows = {}
    for i in range(len(tris)):
        if i in preset565:
            row = tuple(preset565[i])
            color_ref.append((row[0], 0x8000 | preset_rows.setdefault(row, len(preset_rows))))
            continue
        seq = tuple(int(x) for x in colors[:, i])
        c = 0
        if len(set(seq)) > 1:
            c = color_map.setdefault(seq, len(color_map) + 1)
            if c == len(color_pool):
                color_pool.append(seq)
        color_ref.append((seq[0], c))
    assert len(channel_map) < 65536 and len(color_pool) < 65536

    # 7. poses: one per frame of each clip, blending between neighbouring keys of that clip
    poses, clips = [], []
    for ci, (cname, _, _, period, _) in enumerate(config["clips"]):
        first = len(poses)
        ids = clip_keys[ci]
        frames_of = [key_clip[k][1] for k in ids]
        for f in range(clip_span[ci]):
            frame = frames_of[0] + f
            idx = max(i for i, kf in enumerate(frames_of) if kf <= frame)
            idx = min(idx, len(ids) - 2)
            a, b = frames_of[idx], frames_of[idx + 1]
            poses.append((ids[idx], ids[idx + 1], int(round(255 * (frame - a) / (b - a)))))
        clips.append((cname, first, len(poses) - first, period))
    span = len(poses)

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
    preset_bytes = b"".join(struct.pack(f"<{preset_count}H", *row) for row in preset_rows)
    tables = [vert_bytes, tri_bytes, delta_bytes, scale_bytes, color_bytes, group_bytes, pose_bytes, clip_bytes,
              label_bytes, preset_bytes]
    header_size = 144
    offsets, cursor = [], header_size
    for t in tables:
        cursor = (cursor + 3) & ~3
        offsets.append(cursor)
        cursor += len(t)
    source_sha = hashlib.sha256(source_path.read_bytes()).digest()
    header = struct.pack("<4sHH11I10I32s16s", MAGIC, VERSION, 0,
                         total, len(tris), keys, len(pool), len(groups), len(color_pool), len(poses), len(clips),
                         preset_count, len(config["emotes"]), len(preset_rows), *offsets, source_sha,
                         config["name"].encode())
    header = header.ljust(header_size, b"\0")
    blob = bytearray(header)
    for t, off in zip(tables, offsets):
        blob.extend(b"\0" * (off - len(blob)))
        blob.extend(t)
    path = output / f"{config['name']}.hca"
    path.write_bytes(bytes(blob))
    # The firmware embeds the deflated form; track_store.c unpacks it into flash once.
    import zlib
    packer = zlib.compressobj(9, zlib.DEFLATED, -15)
    packed = packer.compress(bytes(blob)) + packer.flush()
    (output / f"{config['name']}.hcz").write_bytes(
        struct.pack("<4sII", b"HCZ1", len(blob), zlib.crc32(bytes(blob))) + packed)

    report = {"name": config["name"], "bytes": len(blob), "vertices": total, "triangles": len(tris), "keys": keys,
              "channels": len(pool), "groups": len(groups), "color_channels": len(color_pool), "poses": len(poses),
              "frames": span, "presets": config["presets"], "emotes": config["emotes"],
              "triangles_per_object": {n: len(reduced[n]["tris"]) for n in names},
              "source_sha256": source_sha.hex(), "hca_sha256": hashlib.sha256(bytes(blob)).hexdigest(),
              "preset_color_rows": len(preset_rows), "clips": [list(c) for c in clips], "key_frames": [list(k) for k in key_clip]}

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
            shown_colors = colors[key].copy()
            for i, row in preset565.items():
                shown_colors[i] = row[preset]
            tiles.append(Image.fromarray(rasterize(pos, tris, shown_colors, vis)))
        cols = 4
        sheet_img = Image.new("RGB", (cols * WIDTH, ((len(tiles) + cols - 1) // cols) * HEIGHT))
        for i, t in enumerate(tiles):
            sheet_img.paste(t, ((i % cols) * WIDTH, (i // cols) * HEIGHT))
        sheet_img.save(output / "preview.png")
    (output / "asset-report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k not in ("triangles_per_object", "key_frames")}))


STEP = int(os.environ.get("HUMAN_STEP", "12"))


def human_clips():
    r125 = "human-character-20261120/motion-review-v125/"
    ik = "human-character-20261120/standing-ik-20261121/"
    return [("turn-seat", None, None, 48, STEP),
            ("turn-stand", r125 + "head-turn-standing.blend", None, 48, STEP),
            ("lean-stand", ik + "girl-forward-lean-sync-v115.blend", None, 48, STEP),
            ("hands-stand", ik + "girl-overhead-stretch-v124.blend", None, 48, STEP),
            ("thumb-stand", ik + "girl-thumb-nose-v106.blend", None, 48, STEP),
            ("twist-stand", r125 + "girl-waist-twist-standing-v103.blend", None, 48, STEP),
            ("step-stand", ik + "standing-right-step-and-reaction-v113.zip::right-step-waist.blend", None, 48, STEP),
            ("lean-seat", r125 + "forward-lean-seated-v115.blend", None, 48, STEP),
            ("hands-seat", r125 + "hands-to-head-seated-v124.blend", None, 48, STEP),
            ("thumb-seat", r125 + "thumb-near-nose-seated-v106.blend", None, 48, STEP),
            ("knee-lean", "gaming-chair-seated-lean-rigid-head-76/chair-idle-lean-rigid-head-v76.blend", None, 48, STEP),
            ("knee-cross", "gaming-chair-knee-thigh-yaw-90/chair-knee-thigh-yaw-v90.blend", None, 48, STEP),
            ("sit", "gaming-chair-quick-release-sit-100/chair-quick-release-v100.blend", None, 48, STEP),
            ("stand-up", "gaming-chair-earlier-hands-stand-99/chair-stand-earlier-hands-v99.blend", None, 48, STEP),
            ("turntable", "turntable::human-character-20261120/chair-back-topdown-v109/chair-rear-topdown-v109.blend", None, 48, 9)]


if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else sys.argv[1:]
    blend = None
    if len(argv) >= 3:
        blend, argv = argv[0], argv[1:]
        bpy.ops.wm.open_mainfile(filepath=blend)
    root = argv[2] if len(argv) > 2 else None
    if argv[0] == "cloud":
        config = CLOUD
    elif argv[0] == "human":
        config = dict(HUMAN, clips=human_clips(), root=root)
        if os.environ.get("HUMAN_VIEW"):
            config["view"] = tuple(float(x) for x in os.environ["HUMAN_VIEW"].split(","))
    else:
        raise SystemExit("unknown character " + argv[0])
    export(config, Path(argv[1]).resolve())
