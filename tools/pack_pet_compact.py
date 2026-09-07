"""Build shared 4bpp bodies, palettes and sparse marking overrides (PPT2).

Pillow is host-only. The runtime can decode a single row without a texture.
"""
from pathlib import Path
import struct
from PIL import Image, ImageDraw

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / 'apps/pet/assets'
EYES = [(16,32,24), (17,36,32), (16,29,27)]


def abgr(c):
    r, g, b = c[:3]
    return (r >> 4) | ((g >> 4) << 4) | ((b >> 4) << 8) | 0xf000


def build():
    # Use the previous, reproducible concept-sheet extraction as input.
    raw = (ASSETS/'pets.bin').read_bytes()
    assert len(raw) == 98304, 'input must be the original ABGR4444 sheet'
    tiles = []
    for i in range(12):
        tile = Image.new('RGBA', (64, 64))
        tile.putdata([tuple(((v >> k) & 15)*17 for k in (0, 4, 8, 12))
                      for v in struct.unpack('<4096H', raw[i*8192:(i+1)*8192])])
        tiles.append(tile)
    bodies, palettes, patches, previews = [], [], [], []
    for species in range(3):
        pixels = list(tiles[species*4].getdata())
        # Transparent margins must not consume an adaptive palette entry.
        visible = Image.new('RGB', (sum(p[3] != 0 for p in pixels), 1))
        visible.putdata([p[:3] for p in pixels if p[3]])
        q = visible.quantize(colors=12, dither=Image.Dither.NONE)
        qp = q.getpalette()
        colors = [tuple(qp[i*3:i*3+3]) for i in range(12)]
        qi = iter(q.getdata())
        base = [next(qi)+1 if p[3] else 0 for p in pixels]
        left,right,ey = EYES[species]
        for ex in (left,right):
            for y in range(ey-3,ey+3):
                for x in range(ex-2,ex+3):
                    base[y*64+x] = base[(ey-4)*64+ex]
        # Tabby stripes are optional markings rather than common geometry.
        stripes = {}
        if species == 0:
            light = min(range(12), key=lambda i: sum((colors[i][k]-[248,191,110][k])**2 for k in range(3)))+1
            for pos, p in enumerate(pixels):
                r, g, b, a = p
                if a and r > 140 and 65 < g < 180 and b < g*.68 and r > g*1.25:
                    stripes[pos] = base[pos]
                    base[pos] = light
        bodies.append(bytes(base[i] | base[i+1] << 4 for i in range(0, 4096, 2)))
        for variant in range(4):
            source = list(tiles[species*4+variant].getdata())
            palette = [0]
            for index in range(1, 13):
                samples = [source[i][:3] for i, b in enumerate(base)
                           if b == index and source[i][3] and i not in stripes]
                if variant == 0 or not samples:
                    c = colors[index-1]
                else:
                    c = tuple(sorted(p[k] for p in samples)[len(samples)//2] for k in range(3))
                palette.append(abgr(c))
            if species == 0 and variant == 1:
                for j,(r,g,b) in enumerate(colors,1):
                    if r>150 and g>100 and b<g*.85:
                        palette[j]=abgr((246,237,217))
            if variant == 3:
                # Ink has several shades from the source art. Lift all of
                # them so the contour stays continuous without a new mask.
                for j,c in enumerate(colors,1):
                    if max(c)<100:
                        palette[j]=abgr((153,187,204) if sum(c)<100 else (136,170,187))
            palette.extend([abgr((238,147,57)), abgr((45,43,43)), abgr((246,43,184))])
            patch = dict(stripes) if species == 0 and variant in (0, 3) else {}
            if species == 0 and variant == 3:
                palette[13] = abgr((0,195,250))
                patch = {p: 13 for p in stripes}
            if species == 0 and variant == 1:
                # Mark the coat only, preserving eyes/mouth and the outline.
                for pos, p in enumerate(source):
                    r, g, b, a = p
                    br, bg, bb, ba = pixels[pos]
                    if not a or not ba or max(br, bg, bb) < 140 or (20 <= pos//64 <= 32 and 13 <= pos%64 <= 36):
                        continue
                    if max(r, g, b) < 95:
                        patch[pos] = 14
                    elif r > 170 and 80 < g < 180 and b < g*.7:
                        patch[pos] = 13
            if variant == 3:
                for pos, p in enumerate(source):
                    r, g, b, a = p
                    if a and base[pos] and r > 140 and b > 90 and g < r*.55:
                        patch[pos] = 15
            for ex in (left,right):
                for y in range(ey-3,ey+3):
                    for x in range(ex-2,ex+3):
                        patch.pop(y*64+x,None)
            palettes.append(struct.pack('<16H', *palette))
            patches.append(b''.join(struct.pack('<H', pos | index << 12) for pos, index in sorted(patch.items())))
            rgba = Image.new('RGBA', (64, 64))
            rgba.putdata([tuple(((palette[patch.get(i, b)] >> k) & 15)*17 for k in (0, 4, 8, 12)) for i, b in enumerate(base)])
            # These eyes are generated by the runtime too, never stored.
            eye = (17,221,255,255) if variant==3 else (255,187,51,255) if species==0 and variant==2 else (51,34,17,255)
            for ex in (left,right):
                for dy in range(-2,3):
                    for dx in range(-2,3):
                        if abs(dx)+abs(dy)<=3:
                            rgba.putpixel((ex+dx,ey+dy),eye)
                rgba.putpixel((ex-1,ey-1),(255,255,238,255))
                if species==0 and variant==2:
                    for dy in range(-1,2): rgba.putpixel((ex,ey+dy),(17,17,17,255))
            previews.append(rgba)
    offsets = [0]
    for patch in patches:
        offsets.append(offsets[-1]+len(patch))
    streams, rows = bytearray(), []
    for body in bodies:
        for y in range(64):
            raw = body[y*32:(y+1)*32]
            indices = [(raw[x//2]>>(x%2*4))&15 for x in range(64)]
            runs = bytearray()
            x = 0
            while x<64:
                count=1
                while count<16 and x+count<64 and indices[x+count]==indices[x]:
                    count+=1
                runs.append((count-1)<<4 | indices[x])
                x+=count
            packed = len(runs)<32
            rows.append(len(streams) | (0 if packed else 0x8000))
            streams.extend(runs if packed else raw)
    rows.append(len(streams))
    data = b'PPT2'+struct.pack('<193H', *rows)+b''.join(palettes)+struct.pack('<13H', *offsets)+streams+b''.join(patches)
    (ASSETS/'pets-compact.bin').write_bytes(data)
    contact = Image.new('RGB', (256, 228), '#172535')
    draw = ImageDraw.Draw(contact)
    labels = ['TABBY','CALICO','BLACK','NEON CAT','PINK','IVORY','MINT','NEON AXO','GRAY','YELLOW','BLUE','NEON BIRD']
    for i, tile in enumerate(previews):
        x, y = i%4*64, i//4*76
        contact.paste(tile, (x, y), tile)
        draw.text((x+2, y+64), labels[i], fill='#ffffff')
    contact.resize((768, 684), Image.Resampling.NEAREST).save(ASSETS/'compact-preview.png')
    print(f'PPT2: {len(data)} bytes; bodies={len(streams)} palettes=384 markings={offsets[-1]} metadata=416')


if __name__ == '__main__':
    build()
