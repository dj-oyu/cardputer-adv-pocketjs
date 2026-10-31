"""Host-only painter/depth oracle for the authored fox sheet, not product Z-buffer."""
import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import zlib

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'tools/character/fox_retopology_fixture.js'
WIDTH, HEIGHT = 240, 135


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def literal(text, pattern):
    found = re.findall(pattern, text)
    assert len(found) == 1, 'expected one explicit authored typed-array literal'
    values = json.loads('[' + found[0] + ']')
    assert all(type(v) in (int, float) and math.isfinite(v) for v in values)
    return values


def geometry(text):
    xyz = literal(text, r'source\.set\(new Float32Array\(\[([^\]]+)\]\)\)')
    indices = literal(text, r'indices\s*=\s*new Uint16Array\(\[([^\]]+)\]\)')
    assert len(xyz) == 30 * 3 and len(indices) == 46 * 3
    assert all(type(i) is int and 0 <= i < 30 for i in indices)
    return ([tuple(map(f32, xyz[i:i + 3])) for i in range(0, len(xyz), 3)],
            [indices[i:i + 3] for i in range(0, len(indices), 3)])


def project(xyz, yaw, pitch):
    # R_y(yaw) R_x(pitch); the existing fixture uses pitch=0. Extended pitch
    # views are explicit oracle samples, not claims about app control support.
    yaw, pitch = f32(yaw * math.pi / 180), f32(pitch * math.pi / 180)
    c, s, cp, sp = map(f32, (math.cos(yaw), math.sin(yaw), math.cos(pitch), math.sin(pitch)))
    zoom = f32(1.3)
    xrow = [f32(zoom * c), f32(zoom * s * sp), f32(zoom * s * cp), 120.0]
    yrow = [0.0, f32(zoom * cp), f32(-zoom * sp), 78.0]
    zrow = [-s, f32(c * sp), f32(c * cp)]

    def dot(row, p):
        # pocket_shape.c project(): float multiply/add, no FMA, final F32 view.
        return f32(f32(f32(f32(row[0] * p[0]) + f32(row[1] * p[1])) +
                       f32(row[2] * p[2])) + row[3])

    return [(dot(xrow, p), dot(yrow, p), f32(sum(a * b for a, b in zip(zrow, p)))) for p in xyz]


def edge(a, b, x, y):
    return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])


def closed(a, b):
    return b[1] < a[1] or (b[1] == a[1] and b[0] > a[0])


def compare(points, triangles):
    areas = [edge(points[a], points[b], points[c][0], points[c][1]) for a, b, c in triangles]
    visible = [i for i, area in enumerate(areas) if area > 1e-5]
    order = sorted(visible, key=lambda i: (sum(points[v][2] for v in triangles[i]) / 3, i))
    paint, nearest = [[-1] * (WIDTH * HEIGHT) for _ in range(2)]
    paint_z, nearest_z = [[-math.inf] * (WIDTH * HEIGHT) for _ in range(2)]
    coverage = [0] * (WIDTH * HEIGHT)
    for tid in order:
        a, b, c = [points[v] for v in triangles[tid]]
        ymin, ymax = max(0, math.floor(min(a[1], b[1], c[1]))), min(HEIGHT, math.ceil(max(a[1], b[1], c[1])))
        xmin, xmax = max(0, math.floor(min(a[0], b[0], c[0]))), min(WIDTH, math.ceil(max(a[0], b[0], c[0])))
        for y in range(ymin, ymax):
            for x in range(xmin, xmax):
                edges = [edge(b, c, x + .5, y + .5), edge(c, a, x + .5, y + .5), edge(a, b, x + .5, y + .5)]
                if any(v < 0 or (v == 0 and not closed(u, w)) for v, (u, w) in zip(edges, [(b, c), (c, a), (a, b)])):
                    continue
                pos = y * WIDTH + x
                z = sum(v * p[2] for v, p in zip(edges, [a, b, c])) / areas[tid]
                coverage[pos] += 1
                paint[pos], paint_z[pos] = tid, z
                if z > nearest_z[pos]:
                    nearest[pos], nearest_z[pos] = tid, z
    mismatch = [i for i in range(WIDTH * HEIGHT)
                if paint[i] != nearest[i] and nearest_z[i] - paint_z[i] > 1e-5]
    pairs = Counter((paint[i], nearest[i]) for i in mismatch)
    result = dict(visible=len(visible), culled=[i for i, a in enumerate(areas) if a <= 1e-5],
                  covered_pixels=sum(t >= 0 for t in nearest), overlap_pixels=sum(n > 1 for n in coverage),
                  mismatch_pixels=len(mismatch),
                  pairs=[dict(painter=a, nearer=b, pixels=n) for (a, b), n in pairs.most_common()],
                  centroid_order=order)
    return result, paint, nearest, set(mismatch)


def png(path, pixels):
    raw = b''.join(b'\0' + bytes(pixels[y * WIDTH * 3:(y + 1) * WIDTH * 3]) for y in range(HEIGHT))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', WIDTH, HEIGHT, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--out-dir', type=Path, default=ROOT / '.cache/fox-retopology-review/tracked')
    parser.add_argument('--images', action='store_true', help='also save 27 diagnostic PNGs')
    args = parser.parse_args()
    before = SOURCE.read_bytes()
    xyz, triangles = geometry(before.decode('utf-8'))
    args.out_dir.mkdir(parents=True, exist_ok=True)
    results = []
    for pitch in (-10, 0, 10):
        for yaw in (-35, 0, 35):
            result, paint, nearest, mismatch = compare(project(xyz, yaw, pitch), triangles)
            result.update(yaw=yaw, pitch=pitch)
            results.append(result)
            print(f"FOX_DEPTH yaw={yaw} pitch={pitch} visible={result['visible']} overlap={result['overlap_pixels']} mismatch={result['mismatch_pixels']}")
            if args.images:
                tag = f'yaw{yaw:+d}_pitch{pitch:+d}'
                for kind, ids in [('painter', paint), ('depth', nearest)]:
                    png(args.out_dir / (tag + '_' + kind + '.png'),
                        [v for tid in ids for v in ((220, 234, 224) if tid < 0 else
                         (50 + tid * 73 % 180, 40 + tid * 107 % 180, 30 + tid * 137 % 180))])
                png(args.out_dir / (tag + '_difference.png'),
                    [v for i in range(WIDTH * HEIGHT) for v in ((255, 20, 20) if i in mismatch else
                     ((85, 85, 85) if nearest[i] >= 0 else (230, 238, 230)))])
    unchanged = before == SOURCE.read_bytes()
    passed = unchanged and len(results) == 9 and all(r['mismatch_pixels'] == 0 and r['covered_pixels'] > 0 for r in results)
    # Require actual occlusion samples; disjoint triangles alone cannot prove ordering.
    passed = passed and all(r['overlap_pixels'] > 0 for r in results if r['yaw'] != 0)
    proof = dict(kind='HOST DEPTH ORACLE / NOT DEVICE OR PRODUCT Z-BUFFER', passed=passed,
                 source=str(SOURCE.relative_to(ROOT)), source_sha256=hashlib.sha256(before).hexdigest(),
                 test_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), sources_unchanged=unchanged,
                 vertices=len(xyz), triangles=len(triangles), views=results,
                 projection='Ry(yaw)*Rx(pitch), F32 screen projection, ortho zoom=F32(1.3), center=(120,78)',
                 coverage='pixel-center, half-open top-left, signed screen area >1e-5',
                 depth='larger viewZ nearer; double barycentric reference over F32 vertices; non-tie tolerance=1e-5',
                 scope='Nine samples of this sheet only; excludes separate eyes/nose/ears, continuous-angle guarantee and image quality acceptance.')
    (args.out_dir / 'result.json').write_text(json.dumps(proof, indent=2) + '\n')
    assert passed, 'painter/depth mismatch, missing occlusion sample or changed fixture; inspect result.json'
    print('FOX_DEPTH_PASS views=9 vertices=30 triangles=46 mismatch_pixels=0 source_sha256=' + proof['source_sha256'])


if __name__ == '__main__':
    main()
