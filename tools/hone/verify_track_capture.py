"""Compare the native RGB565 capture with an independent integer-depth image oracle."""
import argparse
import array
import ast
import json
from pathlib import Path
import struct
import sys
import zipfile


def read_array(bundle, name):
    with bundle.open(name + '.npy') as f:
        if f.read(8) != b'\x93NUMPY\x01\x00':
            raise ValueError('Expected NPY 1.0')
        header = ast.literal_eval(f.read(struct.unpack('<H', f.read(2))[0]).decode('ascii'))
        formats = {'<i2': 'h', '<u2': 'H', '<i8': 'q', '<i4': 'i', '|b1': 'B'}
        values = array.array(formats[header['descr']])
        values.frombytes(f.read())
        if sys.byteorder != 'little':
            values.byteswap()
        return values, header['shape'], header['fortran_order']


def edge(a, b, x, y):
    return (b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (x - a[0])


def closed(a, b):
    return b[1] < a[1] or (b[1] == a[1] and b[0] > a[0])


def image(vertices, triangles, visible, color):
    result = [0] * (240 * 135)
    tied_colors = {}
    # Exact rational depth comparison avoids sharing the C float implementation.
    depth = [(-32769, 1)] * len(result)
    for tri, enabled, rgb in zip(triangles, visible, color):
        if not enabled:
            continue
        a, b, c = (vertices[i] for i in tri)
        area = edge(a, b, c[0], c[1])
        if area == 0:
            continue
        if area < 0:
            a, b, area = b, a, -area
        edges = [(b,c), (c,a), (a,b)]
        limits = [0 if closed(u,v) else 1 for u,v in edges]
        x0 = max(0, -(-min(a[0],b[0],c[0])//16))
        x1 = min(239, max(a[0],b[0],c[0])//16)
        for y in range(max(0,-(-min(a[1],b[1],c[1])//16)), min(134,max(a[1],b[1],c[1])//16)+1):
            for x in range(x0,x1+1):
                w0, w1, w2 = (edge(u,v,16*x,16*y) for u,v in edges)
                if w0 < limits[0] or w1 < limits[1] or w2 < limits[2]:
                    continue
                z = w0*a[2]+w1*b[2]+w2*c[2]
                which = y*240+x
                old, denominator = depth[which]
                delta = z*denominator - old*area
                if delta > 0:
                    result[which] = rgb
                    depth[which] = z, area
                    tied_colors.pop(which,None)
                elif delta == 0 and rgb != result[which]:
                    tied_colors.setdefault(which,set()).add(rgb)
    return result, tied_colors


def compare(actual, reference):
    return (sum(a != b for a,b in zip(actual,reference)),
            sum(bool(a) != bool(b) for a,b in zip(actual,reference)),
            sum(bool(a or b) for a,b in zip(actual,reference)))


def interpolate(a, b, w):
    return (a*(255-w)+b*w+127)//255


def interpolate_color(a, b, w):
    return (interpolate(a>>11,b>>11,w)<<11 |
            interpolate((a>>5)&63,(b>>5)&63,w)<<5 |
            interpolate(a&31,b&31,w))


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('export_dir', type=Path)
    p.add_argument('--all-poses', action='store_true', help='include every interpolated and held frame')
    p.add_argument('--strict-first-face', action='store_true', help='also fail on mathematically equal-depth face-order differences')
    args = p.parse_args()
    d = args.export_dir
    report = json.loads((d/'asset-report.json').read_text())
    with zipfile.ZipFile(d/'track-preview.npz') as bundle:
        xyz, (keys, count, dimensions), col = read_array(bundle, 'positions')
        tris, (faces, n), tc = read_array(bundle, 'triangles')
        mask, _, mc = read_array(bundle, 'visible')
        rgb, _, cc = read_array(bundle, 'colors')
        poses, (frames, _), pc = read_array(bundle, 'poses')
    assert dimensions == n == 3 and mc and cc and not (pc or col or tc)
    triples = list(zip(tris[::3], tris[1::3], tris[2::3]))
    scheduled = []
    seen_keys = set()
    for i in range(frames):
        a,b,w = poses[3*i:3*i+3]
        key = b if w == 255 else a
        unblended = not w or a == b or w == 255
        if args.all_poses or (unblended and key not in seen_keys):
            scheduled.append((i,a,b,w))
        if unblended:
            seen_keys.add(key)
    stride = count*3
    base = xyz[len(report['inspection_names'])*stride:(len(report['inspection_names'])+1)*stride]
    steps = [max(1, (max(abs(value-q) for value in xyz[i::stride])+126)//127) for i,q in enumerate(base)]
    raw_keys = [xyz[k*stride:(k+1)*stride] for k in range(keys)]
    encoded_keys = [[b+s*round((v-b)/s) for b,s,v in zip(base,steps,raw)] for raw in raw_keys]
    # Hidden faces carry their nearest visible sample's color through a cut.
    color_keys = [[0]*faces for _ in range(keys)]
    for face in range(faces):
        seen = [k for k in range(keys) if mask[face*keys+k]]
        for k in range(keys):
            source = k if mask[face*keys+k] else min(seen,key=lambda other: abs(other-k))
            color_keys[k][face] = rgb[face*keys+source]
    aggregate = {k:[0,0,0] for k in ['uncompressed','encoded']}
    encoded_ties = 0
    with (d/'frames.rgb565').open('rb') as f:
        assert f.seek(0,2) == frames*64800
        for pose, a, b, w in scheduled:
            f.seek(pose*64800)
            actual = struct.unpack('<32400H',f.read(64800))
            color = [interpolate_color(c,d,w) for c,d in zip(color_keys[a],color_keys[b])]
            key = a if w < 128 else b
            for group, coordinates in [('uncompressed',raw_keys),('encoded',encoded_keys)]:
                v = [interpolate(c,d,w) for c,d in zip(coordinates[a],coordinates[b])]
                vertices = zip(v[::3],v[1::3],v[2::3])
                ref, tied_colors = image(list(vertices),triples,mask[key::keys],color)
                counts = compare(actual,ref)
                aggregate[group] = [a+b for a,b in zip(aggregate[group],counts)]
                if group == 'encoded':
                    encoded_ties += sum(actual[i] in permitted for i,permitted in tied_colors.items())
    measures = ('rgb_mismatches', 'silhouette_mismatches', 'union_pixels')
    print(json.dumps({'poses':len(scheduled), 'distinct_unblended_keys':len(seen_keys), 'total_pixels':len(scheduled)*32400,
                      'encoded_front_depth_tie_color_differences':encoded_ties,
                      **{k:dict(zip(measures,v)) for k,v in aggregate.items()}}))
    if aggregate['encoded'][1] or aggregate['encoded'][0] > (0 if args.strict_first_face else encoded_ties):
        raise SystemExit('encoded RGB565 mismatch against exact depth reference')


if __name__ == '__main__':
    main()
