#!/usr/bin/env python3
"""Rewrites a trained .gcda for the source tree that builds with it
(docs/vm/qjs-pgo.md 1.1).

GCC finds a function's counts by an ident that, for a static function, hashes
the source path as given on the command line and the object name:

    ident  = crc(crc(crc(0, file), name), aux) & 0x7fffffff    (static)
    ident  = crc(0, name) & 0x7fffffff                         (public)
    lineno = crc(crc(line, file), name)                        (all)

where file is the absolute path of the file that defines the function, aux the
object path given to -o without ".obj", and crc GCC's crc32_string (MSB-first
CRC-32, polynomial 0x04c11db7, the terminating NUL included). Checked against
the .gcno of esp-15.2.0. So a profile trained in one tree matches no static
function in another ("profile for function ... not found"), and a Windows
build tree never has the training tree's paths. This keeps the counts and
recomputes both checksums for the tree at hand.

    rekey.py <in.gcda> <in.gcda.ids> <component dir> <aux> <out.gcda>

The .ids file (tools/pgo/train.sh) lists, per trained function:
    ident line file-relative-to-the-component name
The -o path's separator is not known here (Ninja on Windows may pass either),
so a static function is written once per spelling of aux.
"""
import struct, sys

MASK = 0x7fffffff
TAG_FUNCTION = 0x01000000


def crc_str(c, s):
    for ch in s.encode() + b'\0':
        v = ch << 24
        for _ in range(8):
            fb = 0x04c11db7 if (v ^ c) & 0x80000000 else 0
            c = ((c << 1) & 0xffffffff) ^ fb
            v = (v << 1) & 0xffffffff
    return c


def read_records(data):
    """Header (4 words), [(tag, length word, payload)] and the end marker (a
    zero word). Little-endian (gcov-tool on the host)."""
    if data[:4] != b'adcg':
        raise SystemExit('rekey: not a little-endian .gcda')
    head = data[:16]
    recs, at = [], 16
    while at + 8 <= len(data):
        tag, length = struct.unpack_from('<II', data, at)
        # An all-zero counter record has a negative length and no payload.
        n = 0 if length & 0x80000000 else length
        recs.append((tag, length, data[at + 8:at + 8 + n]))
        at += 8 + n
    tail = data[at:]
    if tail not in (b'', b'\0\0\0\0'):
        raise SystemExit('rekey: trailing bytes')
    return head, recs, tail


def main():
    src, ids_path, comp, aux, out = sys.argv[1:6]
    comp = comp.replace('\\', '/').rstrip('/')
    ids = {}
    for line in open(ids_path):
        p = line.split(None, 3)
        if len(p) == 4:
            ids[int(p[0])] = (int(p[1]), p[2], p[3].strip())
    head, recs, tail = read_records(open(src, 'rb').read())
    auxes = [aux.replace('\\', '/')]
    if '/' in auxes[0]:
        auxes.append(auxes[0].replace('/', '\\'))
    # Group each FUNCTION record with the counter records that follow it.
    groups, cur, pre = [], None, []
    for r in recs:
        if r[0] == TAG_FUNCTION:
            cur = [r]
            groups.append(cur)
        elif cur is None:
            pre.append(r)
        else:
            cur.append(r)
    outrecs = list(pre)
    known = unknown = 0
    for g in groups:
        tag, length, pay = g[0]
        if len(pay) < 12:
            outrecs.extend(g); continue
        ident, lineno, cfg = struct.unpack_from('<III', pay)
        info = ids.get(ident)
        if info is None:
            unknown += 1; outrecs.extend(g); continue
        known += 1
        line, rel, name = info
        path = f'{comp}/{rel}'
        new_lineno = crc_str(crc_str(line, path), name)
        public = ident == crc_str(0, name) & MASK
        idents = [ident] if public else sorted({crc_str(crc_str(crc_str(0, path), name), a) & MASK
                                                for a in auxes})
        for i in idents:
            outrecs.append((tag, length, struct.pack('<III', i, new_lineno, cfg) + pay[12:]))
            outrecs.extend(g[1:])
    with open(out, 'wb') as f:
        f.write(head)
        for tag, length, pay in outrecs:
            f.write(struct.pack('<II', tag, length) + pay)
        f.write(tail)
    if unknown:
        print(f'rekey: {src}: {unknown} of {known + unknown} functions not in {ids_path}', file=sys.stderr)


if __name__ == '__main__':
    main()
