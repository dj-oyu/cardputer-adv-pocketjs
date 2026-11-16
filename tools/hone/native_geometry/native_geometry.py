"""Compile source corner geometry to immutable C data; not runtime admission.

No material, rig, transform, modifier or animation semantics are inferred.
The original source package remains authoritative and is required alongside it.
"""
import hashlib
import json
import math
import pathlib
import struct
from consumer_plan import prepare

HEADER = '''/* Generated private geometry candidate. Do not publish asset payloads.
 * Not runtime-ready: source package and semantic adapters remain required. */
#ifndef HONE_PRIVATE_GEOMETRY_CANDIDATE_H
#define HONE_PRIVATE_GEOMETRY_CANDIDATE_H
#include <stdint.h>
typedef struct { uint32_t group; float weight; } hone_input_weight;
typedef struct { uint32_t first, count; } hone_input_range;
typedef struct {
 uint32_t vertices, triangles, morphs, groups, relative_morph;
 const float *position, *normal;
 const uint32_t *index, *source_vertex, *source_corner, *source_polygon;
 const uint8_t *smooth;
 const hone_input_range *weight_ranges;
 const hone_input_weight *weights;
 const float *morph_positions;
 const uint32_t *morph_relative;
} hone_input_mesh;
'''

def f32(x):
    try:
        value = struct.unpack('<f', struct.pack('<f', x))[0]
    except (OverflowError, struct.error) as error:
        raise ValueError('float32 overflow') from error
    if not math.isfinite(value):
        raise ValueError('nonfinite float32')
    return value.hex() + 'f'

def emit(package):
    plan = prepare(package)
    if len(plan['meshes']) > 1024:
        raise ValueError('mesh bound')
    out = [HEADER]
    records = []
    def array(kind, name, values):
        # A standard C nonzero sentinel for empty arrays, never included in count.
        out.append(f'static const {kind} {name}[] = {{' + ','.join(values or ['0']) + '};\n')
    for n, (mesh, original) in enumerate(zip(plan['meshes'], package['meshes'])):
        prefix = f'hone_input_{n}'
        count = len(mesh['positions'])
        if not count or count > 1000000 or len(mesh['triangles']) > 1000000:
            raise ValueError('geometry bound')
        def floats(key, data):
            array('float', prefix+'_'+key, [f32(x) for row in data for x in row])
        def ints(key, data, kind='uint32_t'):
            array(kind, prefix+'_'+key, [str(int(x)) for x in data])
        floats('position', mesh['positions']); floats('normal', mesh['rest_corner_normals'])
        ints('index', [x for t in mesh['triangles'] for x in t])
        for dest, src in [('source_vertex','source_vertices'),('source_corner','source_corners'),('source_polygon','source_polygons'),('smooth','polygon_smooth')]:
            ints(dest, mesh[src], 'uint8_t' if dest == 'smooth' else 'uint32_t')
        ranges=[]; weights=[]
        for row in mesh['weights']:
            ranges.append('{'+str(len(weights))+','+str(len(row))+'}')
            weights.extend('{'+str(g)+','+f32(w)+'}' for g,w in row)
        array('hone_input_range',prefix+'_weight_ranges',ranges)
        array('hone_input_weight',prefix+'_weights',weights or ['{0,0}'])
        keys=mesh['shape_keys']['blocks'] if mesh['shape_keys'] else []
        if len(keys)>4096:
            raise ValueError('morph bound')
        names=[k['name'] for k in keys]
        floats('morph_positions',[p for k in keys for p in k['positions']])
        ints('morph_relative',[names.index(k['relative_key']) for k in keys])
        fields=['position','normal','index','source_vertex','source_corner','source_polygon','smooth','weight_ranges','weights','morph_positions','morph_relative']
        records.append('{'+','.join([str(count),str(len(mesh['triangles'])),str(len(keys)),str(len(original['vertex_groups'])),str(int(bool(mesh['shape_keys'] and mesh['shape_keys'].get('use_relative') is True)))]+[prefix+'_'+x for x in fields])+'}')
    array('hone_input_mesh','hone_input_meshes',records or ['{0}'])
    out.append(f'static const uint32_t hone_input_mesh_count = {len(records)};\n')
    out.append('#endif\n')
    return ''.join(out)

def build(source, target):
    raw=pathlib.Path(source).read_bytes()
    content=emit(json.loads(raw)).encode()
    # No overwrite: a new candidate cannot silently replace a published input.
    with pathlib.Path(target).open('xb') as stream:
        stream.write(content)
    return {'runtime_ready':False,'source_package_sha256':hashlib.sha256(raw).hexdigest(),
            'candidate_sha256':hashlib.sha256(content).hexdigest(),'bytes':len(content),
            'numeric_format':'IEEE754 float32, explicit rounding',
            'remaining':['material/opacity','transform and skin evaluation','morph controls','normal recomputation','runtime registration and lifetime','same-head build and hardware']}

if __name__ == '__main__':
    import argparse
    p=argparse.ArgumentParser();p.add_argument('source');p.add_argument('output');a=p.parse_args()
    print(json.dumps(build(a.source,a.output),indent=2))
