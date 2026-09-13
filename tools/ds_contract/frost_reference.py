"""Independent RGB565 filter reference and device gallery pixel expectation."""
import struct
import sys


def pack(rgb):
    r, g, b = rgb
    return (r >> 3) << 11 | (g >> 2) << 5 | b >> 3


def unpack(p):
    r, g, b = p >> 11, p >> 5 & 63, p & 31
    return ((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))


def pattern(x, y):
    if y < 16:
        return pack((13, 23, 39))
    return pack((30, 160, 220) if ((x % 120) // 12 + y // 12) % 2 else (235, 128, 48))


def snapshot(radius):
    image = []
    for y in range(0, 135, 8):
        row = []
        for x in range(0, 240, 8):
            pixels = [unpack(pattern(px, py)) for py in range(y, min(y + 8, 135)) for px in range(x, x + 8)]
            row.append(pack(tuple((sum(p[c] for p in pixels) + len(pixels) // 2) // len(pixels) for c in range(3))))
        image.append(row)
    for axis in (0, 1):
        result = []
        for y in range(17):
            row = []
            for x in range(30):
                pixels = [unpack(image[min(16, max(0, y + (d if axis else 0)))][min(29, max(0, x + (0 if axis else d)))])
                          for d in range(-radius, radius + 1)]
                row.append(pack(tuple((sum(p[c] for p in pixels) + len(pixels) // 2) // len(pixels) for c in range(3))))
            result.append(row)
        image = result
    return image


def frost_pixel(image, x, y):
    sx, sy = max(0, min(464, 2*x - 7)), max(0, min(256, 2*y - 7))
    x0, wx = divmod(sx, 16)
    y0, wy = divmod(sy, 16)
    colors = [unpack(image[py][px]) for py in (y0, min(16, y0+1)) for px in (x0, min(29, x0+1))]
    weights = [(16-wx)*(16-wy), wx*(16-wy), (16-wx)*wy, wx*wy]
    rgb = tuple((sum(p[c]*w for p, w in zip(colors, weights)) + 128)//256 for c in range(3))
    return pack(tuple((t*96 + v*159 + 127)//255 for t, v in zip((28, 48, 67), rgb)))


def gallery_pixel(image, x, y):
    p = pattern(x, y)
    local = x % 120
    if 4 <= local < 116 and 24 <= y < 119:
        if x >= 120:
            p = frost_pixel(image, x, y)
        else:
            p = pack(tuple((t*96 + v*159 + 127)//255 for t, v in zip((28, 48, 67), unpack(p))))
        if local in (4, 115) or y in (24, 118):
            p = pack((173, 206, 225))
        if 20 <= local < 92 and 42 <= y < 46:
            p = pack((238, 244, 250))
        if 20 <= local < 76 and 54 <= y < 57:
            p = pack((173, 206, 225))
        if 20 <= local < 100 and 85 <= y < 105:
            p = pack((71, 199, 174))
    if 5 <= y < 11 and (8 <= local < 14 or (x >= 120 and 19 <= local < 25)):
        p = pack((238, 244, 250))
    return p


if __name__ == '__main__':
    data = open(sys.argv[1], 'rb').read()
    assert len(data) == 2*240*135*2
    for radius in (1, 2):
        image = snapshot(radius)
        for y in range(135):
            for x in range(240):
                actual = struct.unpack_from('<H', data, ((radius-1)*32400+y*240+x)*2)[0]
                assert actual == frost_pixel(image, x, y), (radius, x, y, actual)
    print('frost reference: PASS (64800 pixels)')
