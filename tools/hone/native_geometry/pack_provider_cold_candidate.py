"""Pack an admitted, all-provider mesh AYNM v1 into a private provider-only AYNM.

The caller must open the original with ayatsuri_model_open_provider before using
the derivative, and open the result the same way. The original is authoritative.
All mesh positions/normals must come from the provider on every evaluation.
"""
import argparse
import math
import pathlib
import struct


DIRECTORY = struct.Struct("<HHIIII")
STRIDES = {1: 16, 2: 36, 3: 12, 4: 24, 5: 72, 6: 8, 7: 1,
           9: 32, 10: 28, 11: 6, 12: 12, 13: 4}


def require(ok, reason):
    if not ok:
        raise ValueError(reason)


def pack_provider(blob):
    require(len(blob) >= 32, "truncated header")
    magic, version, hdr, size, count, directory, stride, flags, reserved = struct.unpack_from("<4sHH6I", blob)
    require((magic, version, hdr, size, directory, stride, flags, reserved)
            == (b"AYNM", 1, 32, len(blob), 32, 20, 0, 0), "unsupported header")
    require(7 <= count <= len(STRIDES) and 32 + 20*count <= size, "invalid directory")
    sections = {}
    spans = []
    for i in range(count):
        kind, flag, offset, length, n, width = DIRECTORY.unpack_from(blob, 32+20*i)
        require(kind in STRIDES and kind not in sections and flag == 1 and
                width == STRIDES[kind] and length == n*width, "unsupported section")
        require(offset % 8 == 0 and offset >= 32 + 20*count and offset+length <= size, "bad section bounds")
        if length:
            require(all(offset >= end or offset+length <= start for start, end in spans), "overlap")
            spans.append((offset, offset+length))
        sections[kind] = blob[offset:offset+length]
    require(set(STRIDES)-{13} <= sections.keys(), "missing mesh provider sections")
    require(not sections[3] and not sections[4] and not sections[12], "non-mesh shapes/holes")
    require(all(sections[1][at] in (0, 3) for at in range(2, len(sections[1]), 16)), "non-mesh node")
    vertices = sections[10]
    require(bool(vertices), "missing vertices")
    mask = vertices[24:28]
    for at in range(0, len(vertices), 28):
        row = struct.unpack_from("<7f", vertices, at)
        require(all(math.isfinite(v) and abs(v) <= 4096 for v in row)
                and abs(sum(v*v for v in row[3:6])-1) <= 1e-4, "invalid provider cold row")
        require(vertices[at+24:at+28] == mask, "mask is not bitwise constant")
    original_indices = sections[11]
    require(all(v[0] < 32 for v in struct.iter_unpack("<H", original_indices)), "invalid cold index")
    descriptors = bytearray(sections[9])
    unique_lists = {}
    packed_indices = bytearray()
    max_vertices = 0
    for at in range(0, len(descriptors), 32):
        first, nv, tri, nt = struct.unpack_from("<4H", descriptors, at)
        require(3 <= nv <= 32 and 1 <= nt <= 64 and first+nv <= len(vertices)//28
                and tri+nt <= len(original_indices)//6, "bad mesh bounds")
        require(not descriptors[at+10], "mesh holes unavailable")
        faces = original_indices[6*tri:6*(tri+nt)]
        require(all(len(set(t)) == 3 and max(t)<nv for t in struct.iter_unpack("<3H", faces)), "bad triangle")
        if faces not in unique_lists:
            unique_lists[faces] = len(packed_indices)//6
            packed_indices.extend(faces)
        struct.pack_into("<H", descriptors, at, 0)
        struct.pack_into("<H", descriptors, at+4, unique_lists[faces])
        max_vertices = max(max_vertices, nv)
    require(max_vertices>0, "empty mesh set")
    sections[9] = descriptors
    sections[10] = (struct.pack("<6f",0,0,0,0,0,1)+mask)*max_vertices
    sections[11] = packed_indices
    output = bytearray(blob[:32+20*count])
    for i,(kind,data) in enumerate(sections.items()):
        output.extend(bytes(-len(output)%8))
        DIRECTORY.pack_into(output,32+20*i,kind,1,len(output),len(data),len(data)//STRIDES[kind],STRIDES[kind])
        output.extend(data)
    struct.pack_into("<I",output,8,len(output))
    return bytes(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original",type=pathlib.Path)
    parser.add_argument("provider_only",type=pathlib.Path)
    args = parser.parse_args()
    source = args.original.read_bytes()
    packed = pack_provider(source)
    with args.provider_only.open("xb") as out:
        out.write(packed)
    print(f"provider-only AYNM {len(source)} -> {len(packed)} bytes; saved {len(source)-len(packed)}")
