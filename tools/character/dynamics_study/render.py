"""Render host-study state as a small APNG; dependency-free data visualization."""
import argparse
import json
import math
from pathlib import Path
import struct
import zlib

FONT = {
'A':(14,17,17,31,17,17,17),'B':(30,17,17,30,17,17,30),'C':(14,17,16,16,16,17,14),
'D':(30,17,17,17,17,17,30),'E':(31,16,16,30,16,16,31),'F':(31,16,16,30,16,16,16),
'G':(14,17,16,23,17,17,15),'H':(17,17,17,31,17,17,17),'I':(14,4,4,4,4,4,14),
'J':(7,2,2,2,18,18,12),'K':(17,18,20,24,20,18,17),'L':(16,16,16,16,16,16,31),
'M':(17,27,21,21,17,17,17),'N':(17,25,21,19,17,17,17),'O':(14,17,17,17,17,17,14),
'P':(30,17,17,30,16,16,16),'Q':(14,17,17,17,21,18,13),'R':(30,17,17,30,20,18,17),
'S':(15,16,16,14,1,1,30),'T':(31,4,4,4,4,4,4),'U':(17,17,17,17,17,17,14),
'V':(17,17,17,17,17,10,4),'W':(17,17,17,21,21,21,10),'X':(17,17,10,4,10,17,17),
'Y':(17,17,10,4,4,4,4),'Z':(31,1,2,4,8,16,31),
'0':(14,17,19,21,25,17,14),'1':(4,12,4,4,4,4,14),'2':(14,17,1,2,4,8,31),
'3':(30,1,1,14,1,1,30),'4':(2,6,10,18,31,2,2),'5':(31,16,16,30,1,1,30),
'6':(14,16,16,30,17,17,14),'7':(31,1,2,4,8,8,8),'8':(14,17,17,14,17,17,14),
'9':(14,17,17,15,1,1,14),'.':(0,0,0,0,0,12,12),'-':(0,0,0,31,0,0,0),
'+':(0,4,4,31,4,4,0),'=':(0,31,0,31,0,0,0),'/':(1,2,2,4,8,8,16),
':':(0,12,12,0,12,12,0),' ': (0,)*7}
W, H = 840, 310


def pixel(data, x, y, color):
    if 0 <= x < W and 0 <= y < H:
        at = (y * W + x) * 3
        data[at:at + 3] = bytes(color)


def line(data, x0, y0, x1, y1, color, width=1):
    steps = max(1, int(max(abs(x1-x0), abs(y1-y0))))
    for i in range(steps + 1):
        x, y = round(x0+(x1-x0)*i/steps), round(y0+(y1-y0)*i/steps)
        for dy in range(-(width//2), width//2+1):
            for dx in range(-(width//2), width//2+1):
                pixel(data, x+dx, y+dy, color)


def circle(data, x, y, radius, color):
    for i in range(48):
        a = i * math.tau / 48
        pixel(data, round(x+radius*math.cos(a)), round(y+radius*math.sin(a)), color)


def text(data, x, y, value, scale=1, color=(35,45,55)):
    for character in value.upper():
        for row, bits in enumerate(FONT.get(character, FONT[' '])):
            for col in range(5):
                if bits & (16 >> col):
                    for dy in range(scale):
                        for dx in range(scale):
                            pixel(data, x+col*scale+dx, y+row*scale+dy, color)
        x += 6 * scale


def chunk(tag, data):
    return struct.pack('>I', len(data))+tag+data+struct.pack('>I', zlib.crc32(tag+data))


def payload(data):
    return zlib.compress(b''.join(b'\0'+data[y*W*3:(y+1)*W*3] for y in range(H)), 6)


def render(frames, index):
    data = bytearray(bytes((244,248,251)) * (W*H))
    text(data, 12, 8, 'HOST SIMULATION - 30 SAMPLES/S - NOT DEVICE FPS', 2, (145,35,35))
    first = frames['rigid'][index]
    paused = 5.5 <= first['wall_time'] <= 6.5
    text(data, 12, 31, 'WALL %.2F / SIM %.2F%s' % (first['wall_time'],first['time'],' PAUSED' if paused else ''), 2)
    text(data, 520, 35, 'LINK F/Z: DESIGN COEFFICIENTS')
    names = [('rigid','RIGID','I1=1'),('reference','LINK F=1.8','Z=.7 I1=1'),
             ('soft','LINK F=.9','Z=.35 I1=1'),('held','SOFT + HOLD','VIEW OFFSET ONLY')]
    blue, cyan, orange, green = (35,85,150),(35,150,175),(210,80,30),(30,145,65)
    for i, (key, label, parameters) in enumerate(names):
        origin = (i*210+105, 94)
        if i:
            line(data, i*210, 54, i*210, 265, (210,220,230))
        text(data, i*210+12, 58, label, 2)
        text(data, i*210+12, 77, parameters)
        row = frames[key][index]
        scale = 2.
        def screen(x, y): return origin[0]+x*scale, origin[1]+y*scale
        base = screen(row['base_x'], row['base_y'])
        joint = (base[0]+35*math.sin(row['q0'])*scale, base[1]+35*math.cos(row['q0'])*scale)
        end = screen(row['tip_x'],row['tip_y'])
        ref = screen(row['reference_x'],row['reference_y'])
        goal = screen(row['goal_x'],row['goal_y'])
        circle(data, *origin, 4, (180,185,195))
        line(data,*base,*joint,blue,3);line(data,*joint,*end,cyan,3)
        circle(data,*base,3,blue);circle(data,*joint,3,blue)
        circle(data,*ref,6,green);circle(data,*end,3,orange)
        line(data,goal[0]-3,goal[1],goal[0]+3,goal[1],(40,40,40))
        line(data,goal[0],goal[1]-3,goal[0],goal[1]+3,(40,40,40))
        text(data, i*210+12, 252, 'REF ERROR %.3F' % row['residual'])
    text(data, 12, 275, 'GREEN: TRACKING REFERENCE  +: REQUESTED GOAL  ORANGE: TIP', 2)
    text(data, 12, 295, 'HOLD MOVES THE DRAWN BASE; IT DOES NOT CHANGE ANGULAR STATE OR MODEL CONTACT.')
    return data


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--folder',type=Path,default=Path('.cache/dynamics-study'))
    args=parser.parse_args()
    frames=json.loads((args.folder/'animation.json').read_text())
    count=sum(r['wall_time']<=11 for r in frames['rigid'])
    output=bytearray(b'\x89PNG\r\n\x1a\n')
    output+=chunk(b'IHDR',struct.pack('>IIBBBBB',W,H,8,2,0,0,0))
    output+=chunk(b'tEXt',b'Description\0HOST SIMULATION; reference model; not device FPS')
    output+=chunk(b'acTL',struct.pack('>II',count,0));seq=0
    for i in range(count):
        data=render(frames,i)
        output+=chunk(b'fcTL',struct.pack('>IIIIIHHBB',seq,W,H,0,0,1,30,0,0));seq+=1
        packed=payload(data)
        if i==0: output+=chunk(b'IDAT',packed)
        else: output+=chunk(b'fdAT',struct.pack('>I',seq)+packed);seq+=1
        if i in (0,60,120,165,195,240,300):
            png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',W,H,8,2,0,0,0))+chunk(b'IDAT',packed)+chunk(b'IEND',b'')
            (args.folder/('frame-%03d.png'%i)).write_bytes(png)
    output+=chunk(b'IEND',b'')
    (args.folder/'comparison.png').write_bytes(output)
    print('HOST_APNG',count,'frames',len(output),'bytes')


if __name__=='__main__': main()
