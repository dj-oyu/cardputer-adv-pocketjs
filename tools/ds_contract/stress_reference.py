"""Animated load scene reference, independent of native scene implementation."""
from frost_reference import pack, unpack, snapshot, frost_pixel


def triangle(t, span):
    return span - abs(t % (span * 2) - span)


def source(x, y, t):
    p = pack((30, 160, 220) if ((x+t//13)//18+(y+t//19)//18) % 2 else (235, 128, 48))
    a, left, top = triangle(t//7, 255), triangle(t//9, 170), triangle(t//17, 85)
    for sx, sy, color, opacity in ((left, top, (230, 40, 120), a),
                                   (170-left, 85-top, (30, 240, 130), 255-a)):
        if sx <= x < sx+70 and sy <= y < sy+50:
            p = pack(tuple((s*opacity+d*(255-opacity)+127)//255 for s, d in zip(color, unpack(p))))
    return p


def panel(t):
    return 40+triangle(t//23, 70), 20+triangle(t//31, 20), 48+triangle(t//11, 144)


def pixel(image, x, y, t):
    p = source(x, y, t)
    px, py, alpha = panel(t)
    if px <= x < px+112 and py <= y < py+80:
        p = frost_pixel(image, x, y, alpha)
        if x in (px, px+111) or y in (py, py+79):
            p = pack((173, 206, 225))
        if px+16 <= x < px+96 and py+18 <= y < py+22:
            p = pack((238, 244, 250))
        if px+16 <= x < px+88 and py+52 <= y < py+68:
            p = pack((71, 199, 174))
    return p


if __name__ == '__main__':
    import struct
    import sys
    data = open(sys.argv[1], 'rb').read()
    assert len(data) == 3*64800
    for index, t in enumerate((0, 1234, 8765)):
        image = snapshot(1+index%2, lambda x, y: source(x, y, t))
        alpha = panel(t)[2]
        for y in range(135):
            for x in range(240):
                value = struct.unpack_from('<H', data, (index*32400+y*240+x)*2)[0]
                assert value == frost_pixel(image, x, y, alpha), (index, x, y)
    print('animated frost reference: PASS (97200 pixels)')
