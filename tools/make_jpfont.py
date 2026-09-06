#!/usr/bin/env python3
"""Build a "JPF1" 1bpp Japanese glyph table from BDF sources.

    python tools/make_jpfont.py --font shinonome \
        --bdf-dir <dir with shnmk12.bdf shnm6x12r.bdf> -o jpfont12.bin --check
    python tools/make_jpfont.py --font misaki \
        --bdf <misaki_gothic.bdf> -o jpfont8.bin --check --show 漢語

Two fonts, for two jobs. Shinonome 12x12 is the reading face: editor text,
tutorial prose, anything a person looks at for more than a moment. Misaki
8x8 is for panels with no room for it, such as the Playground's console
strip, where 33 px holds four 8 px lines but only two 12 px ones.

Source fonts:

  Shinonome (東雲) 0.9.11, /efont/ Electronic Font Open Laboratory,
  http://openlab.ring.gr.jp/efont/ . Fetched as the Debian upstream
  tarball xfonts-shinonome_0.9.11.orig.tar.gz. The archive's LICENSE
  states that every font, document and script in it is Public Domain (the
  listed authors declare they will not exercise their rights), and permits
  modification, conversion to other formats, embedding and redistribution,
  without warranty.
    shnmk12.bdf   JIS X 0208 (1983/1990), 12x12, 6,879 glyphs. ENCODING is
                  the JIS row/cell code (0x2121..), converted to Unicode by
                  setting bit 7 on both bytes and decoding as EUC-JP.
    shnm6x12r.bdf JIS X 0201, 6x12, 256 glyphs, ENCODING is the byte value.
                  Only 0x20-0x7E (ASCII) is taken.

  Misaki (美咲フォント) by Num Kadoma, https://littlelimit.net/misaki.htm .
  Its readme permits use, modification, copying and redistribution freely,
  for personal or commercial purposes, without warranty.
    misaki_gothic.bdf  ISO10646 encoded, so ENCODING is the Unicode scalar
                  directly. Full-width glyphs are 7x7 ink inside an 8x8
                  cell (DWIDTH 8). Its half-width forms are 3 px wide,
                  narrower than the 5x7 face the panels already use for
                  latin, so only the full-width glyphs are taken and
                  ASCII keeps its existing renderer.

Output format "JPF1", little-endian, all offsets from the image start:

  off  0  u32  magic      0x3146504A  'J','P','F','1'
  off  4  u16  version    1
  off  6  u16  cell_w     12
  off  8  u16  cell_h     12
  off 10  u16  baseline   px from the cell top to the baseline (10)
  off 12  u32  count      glyphs, gid 0 = tofu box (mapped to U+0000)
  off 16  u32  cmap_off   32
  off 20  u32  bitmap_off 4-byte aligned
  off 24  u32  bitmap_len count * 24
  off 28  u32  crc32      CRC-32/ISO-HDLC (zlib, seed 0) of image[32..end)
  off 32  cmap: count x 4 bytes, ascending codepoint
            +0 u16 codepoint (BMP)   +2 u8 advance (12 / 6)   +3 u8 flags (0)
          gid == cmap index; the cmap has exactly `count` entries.
          bitmap: gid-linear, 2 bytes per row x 12 rows, top row first,
          MSB is the leftmost pixel, 12 bits used, low 4 bits zero.

Standard library only.
"""
import argparse
import os
import struct
import sys
import zlib

MAGIC = 0x3146504A
VERSION = 1
HDR_SIZE = 32
CMAP_ENTRY = 4


def row_bytes(cell_w):
    return (cell_w + 7) // 8


def glyph_bytes(cell_w, cell_h):
    return row_bytes(cell_w) * cell_h


def parse_bdf(path):
    """Yield (encoding, bbx(w,h,xoff,yoff), rows[int]) per glyph, plus font metrics."""
    glyphs = []
    ascent = descent = None
    enc = bbx = None
    rows = None
    in_bitmap = False
    with open(path, "r", encoding="latin-1") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if in_bitmap:
                if line == "ENDCHAR":
                    glyphs.append((enc, bbx, rows))
                    in_bitmap = False
                    enc = bbx = rows = None
                else:
                    rows.append(int(line, 16))
                continue
            if line.startswith("FONT_ASCENT "):
                ascent = int(line.split()[1])
            elif line.startswith("FONT_DESCENT "):
                descent = int(line.split()[1])
            elif line.startswith("ENCODING "):
                enc = int(line.split()[1])
            elif line.startswith("BBX "):
                w, h, xo, yo = (int(v) for v in line.split()[1:5])
                bbx = (w, h, xo, yo)
            elif line == "BITMAP":
                rows = []
                in_bitmap = True
    if ascent is None or descent is None:
        raise ValueError("%s: missing FONT_ASCENT/FONT_DESCENT" % path)
    return ascent, descent, glyphs


def place(bbx, rows, ascent, cell_w, cell_h):
    """Place a BDF glyph into the cell honouring its BBX.

    BDF bitmap rows are the glyph's bounding box, top row first, MSB left,
    padded to whole bytes. The box's bottom-left corner sits at
    (xoff, yoff) relative to the baseline origin (y up). Cell row r
    (0 = top) corresponds to baseline-relative y = ascent - 1 - r, so the
    box's top row lands on cell row ascent - (yoff + h).
    """
    w, h, xo, yo = bbx
    cell = [0] * cell_h
    src_bytes = (w + 7) // 8
    mask = (1 << cell_w) - 1
    top = ascent - (yo + h)
    for i, bits in enumerate(rows):
        r = top + i
        if r < 0 or r >= cell_h:
            continue
        # Left-align the row's w significant bits at bit (src_bytes*8 - 1).
        v = bits >> (src_bytes * 8 - w)  # exact w-bit value, MSB = leftmost
        v &= (1 << w) - 1
        # Shift into the cell row: leftmost pixel at bit cell_w-1.
        shift = cell_w - w - xo
        if shift >= 0:
            v <<= shift
        else:
            v >>= -shift
        cell[r] |= v & mask
    return cell


def tofu(cell_w, cell_h):
    full = (1 << cell_w) - 1
    edge = (1 << (cell_w - 1)) | 1
    return [full if r in (0, cell_h - 1) else edge for r in range(cell_h)]


def jis_to_unicode(enc):
    b = bytes([(enc >> 8) | 0x80, (enc & 0xFF) | 0x80])
    try:
        s = b.decode("euc_jp")
    except UnicodeDecodeError:
        return None
    return ord(s) if len(s) == 1 else None


def collect_shinonome(bdf_dir):
    """12x12 kanji plus the 6x12 ASCII forms, for reading."""
    asc_k, _, kanji = parse_bdf(os.path.join(bdf_dir, "shnmk12.bdf"))
    asc_r, _, roman = parse_bdf(os.path.join(bdf_dir, "shnm6x12r.bdf"))
    if asc_k != asc_r:
        raise ValueError("ascent differs between the two fonts")
    cell_w = cell_h = 12
    table = {}  # codepoint -> (advance, cell)
    for enc, bbx, rows in roman:
        if 0x20 <= enc <= 0x7E:
            table[enc] = (6, place(bbx, rows, asc_r, cell_w, cell_h))
    dropped = 0
    for enc, bbx, rows in kanji:
        cp = jis_to_unicode(enc)
        if cp is None or cp > 0xFFFF:
            dropped += 1
            continue
        if cp in table:
            continue  # ASCII wins (JIS X 0208 has no ASCII anyway)
        table[cp] = (12, place(bbx, rows, asc_k, cell_w, cell_h))
    return cell_w, cell_h, asc_k, table, dropped


def collect_misaki(path):
    """Full-width 8x8 only. Misaki's own latin forms are 3 px wide, thinner
    than the 5x7 face the panels already draw ASCII with, so they are left
    out and every codepoint below 0x80 keeps its existing renderer."""
    ascent, _, glyphs = parse_bdf(path)
    cell_w = cell_h = 8
    table = {}
    dropped = 0
    for enc, bbx, rows in glyphs:
        if enc < 0 or enc > 0xFFFF:
            dropped += 1
            continue
        if enc < 0x80:
            continue        # latin stays with the 5x7 face
        table[enc] = (8, place(bbx, rows, ascent, cell_w, cell_h))
    return cell_w, cell_h, ascent, table, dropped


def build(cell_w, cell_h, baseline, table, dropped):
    gbytes = glyph_bytes(cell_w, cell_h)
    rbytes = row_bytes(cell_w)
    cps = sorted(table)
    # gid 0 is the tofu box. It is mapped to U+0000 so that the cmap has
    # exactly `count` entries, stays strictly ascending, and gid == index.
    glyphs = [(0, cell_w, tofu(cell_w, cell_h))] + [(cp,) + table[cp] for cp in cps]
    count = len(glyphs)
    cmap_off = HDR_SIZE
    bitmap_off = (cmap_off + count * CMAP_ENTRY + 3) & ~3
    bitmap_len = count * gbytes
    body = bytearray()
    for cp, adv, cell in glyphs:
        body += struct.pack("<HBB", cp, adv, 0)
    body += b"\0" * (bitmap_off - cmap_off - len(body))
    for cp, adv, cell in glyphs:
        for row in cell:
            # Left-aligned in the row's bytes: leftmost pixel is the MSB.
            body += (row << (rbytes * 8 - cell_w)).to_bytes(rbytes, "big")
    hdr = struct.pack("<IHHHHIIIII", MAGIC, VERSION, cell_w, cell_h, baseline,
                      count, cmap_off, bitmap_off, bitmap_len,
                      zlib.crc32(bytes(body)) & 0xFFFFFFFF)
    assert len(hdr) == HDR_SIZE
    return bytes(hdr + body), dropped


def parse_image(img):
    (magic, ver, cw, ch, base, count, cmap_off, bitmap_off, bitmap_len,
     crc) = struct.unpack_from("<IHHHHIIIII", img, 0)
    if magic != MAGIC or ver != VERSION:
        raise ValueError("bad magic/version")
    if zlib.crc32(img[HDR_SIZE:]) & 0xFFFFFFFF != crc:
        raise ValueError("CRC mismatch")
    if bitmap_len != count * glyph_bytes(cw, ch) or bitmap_off + bitmap_len != len(img):
        raise ValueError("bitmap section size mismatch")
    if bitmap_off % 4:
        raise ValueError("bitmap_off not 4-byte aligned")
    cmap = []
    for i in range(count):
        cp, adv, flags = struct.unpack_from("<HBB", img, cmap_off + i * CMAP_ENTRY)
        cmap.append((cp, adv, flags))
    for a, b in zip(cmap, cmap[1:]):
        if a[0] >= b[0]:
            raise ValueError("cmap not strictly ascending at U+%04X" % b[0])
    return dict(cell_w=cw, cell_h=ch, baseline=base, count=count,
                cmap=cmap, bitmap_off=bitmap_off)


def lookup(info, img, cp):
    cmap = info["cmap"]
    lo, hi = 0, len(cmap)
    while lo < hi:
        mid = (lo + hi) // 2
        if cmap[mid][0] < cp:
            lo = mid + 1
        else:
            hi = mid
    if lo < len(cmap) and cmap[lo][0] == cp:
        gid, adv = lo, cmap[lo][1]
    else:
        gid, adv = 0, info["cell_w"]
    cw, ch = info["cell_w"], info["cell_h"]
    rbytes = row_bytes(cw)
    off = info["bitmap_off"] + gid * glyph_bytes(cw, ch)
    rows = []
    for r in range(ch):
        v = int.from_bytes(img[off + r * rbytes:off + (r + 1) * rbytes], "big")
        rows.append(v >> (rbytes * 8 - cw))
    return gid, adv, rows


def art(rows, adv, cell_w):
    out = []
    for r in rows:
        out.append("".join("#" if r & (1 << (cell_w - 1 - x)) else "."
                           for x in range(cell_w)))
    out.append("adv=%d" % adv)
    return "\n".join(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", choices=("shinonome", "misaki"), default="shinonome")
    ap.add_argument("--bdf-dir", help="shinonome: directory holding the two BDFs")
    ap.add_argument("--bdf", help="misaki: the single BDF")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--check", action="store_true", help="re-parse and verify")
    ap.add_argument("--show", default="", help="characters to print as ASCII art")
    args = ap.parse_args()

    if args.font == "shinonome":
        if not args.bdf_dir:
            ap.error("--font shinonome needs --bdf-dir")
        cw, ch, baseline, table, dropped = collect_shinonome(args.bdf_dir)
    else:
        if not args.bdf:
            ap.error("--font misaki needs --bdf")
        cw, ch, baseline, table, dropped = collect_misaki(args.bdf)

    img, dropped = build(cw, ch, baseline, table, dropped)
    parent = os.path.dirname(os.path.abspath(args.output))
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(args.output, "wb") as f:
        f.write(img)
    info = parse_image(img)
    print("wrote %s: %d bytes, %dx%d, %d glyphs (incl. tofu), cmap %d, "
          "bitmap %d, baseline %d, dropped %d"
          % (args.output, len(img), info["cell_w"], info["cell_h"],
             info["count"], len(info["cmap"]) * CMAP_ENTRY,
             info["count"] * glyph_bytes(cw, ch), info["baseline"], dropped))
    if args.check:
        with open(args.output, "rb") as f:
            back = f.read()
        parse_image(back)
        print("check: magic/version/CRC/sizes/cmap order OK")
    for c in args.show:
        gid, adv, rows = lookup(info, img, ord(c))
        print("U+%04X %r gid=%d" % (ord(c), c, gid))
        print(art(rows, adv, info["cell_w"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
