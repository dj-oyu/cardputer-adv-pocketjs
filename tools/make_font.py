"""Generate original, tiny 5x7 glyphs for the shell and PocketJS DCFA atlases."""
import struct
import sys
from pathlib import Path

# Five-bit rows, top to bottom. Lowercase shares uppercase shapes in M1.
GLYPHS = {
 ' ': [0]*7, 'A':[14,17,17,31,17,17,17], 'B':[30,17,17,30,17,17,30],
 'C':[14,17,16,16,16,17,14], 'D':[30,17,17,17,17,17,30],
 'E':[31,16,16,30,16,16,31], 'F':[31,16,16,30,16,16,16],
 'G':[14,17,16,23,17,17,15], 'H':[17,17,17,31,17,17,17],
 'I':[14,4,4,4,4,4,14], 'J':[7,2,2,2,18,18,12],
 'K':[17,18,20,24,20,18,17], 'L':[16,16,16,16,16,16,31],
 'M':[17,27,21,21,17,17,17], 'N':[17,25,25,21,19,19,17],
 'O':[14,17,17,17,17,17,14], 'P':[30,17,17,30,16,16,16],
 'Q':[14,17,17,17,21,18,13], 'R':[30,17,17,30,20,18,17],
 'S':[15,16,16,14,1,1,30], 'T':[31,4,4,4,4,4,4],
 'U':[17,17,17,17,17,17,14], 'V':[17,17,17,17,17,10,4],
 'W':[17,17,17,21,21,21,10], 'X':[17,17,10,4,10,17,17],
 'Y':[17,17,10,4,4,4,4], 'Z':[31,1,2,4,8,16,31],
 '0':[14,17,19,21,25,17,14], '1':[4,12,4,4,4,4,14],
 '2':[14,17,1,2,4,8,31], '3':[30,1,1,14,1,1,30],
 '4':[2,6,10,18,31,2,2], '5':[31,16,16,30,1,1,30],
 '6':[14,16,16,30,17,17,14], '7':[31,1,2,4,8,8,8],
 '8':[14,17,17,14,17,17,14], '9':[14,17,17,15,1,1,14],
 '!':[4,4,4,4,4,0,4], '?':[14,17,1,2,4,0,4],
 '.':[0,0,0,0,0,0,4], ',':[0,0,0,0,0,4,8],
 ':':[0,4,0,0,4,0,0], '/':[1,2,2,4,8,8,16],
 '-':[0,0,0,31,0,0,0], '+':[0,4,4,31,4,4,0],
 '>':[16,8,4,2,4,8,16], '<':[1,2,4,8,4,2,1],
 '[':[14,8,8,8,8,8,14], ']':[14,2,2,2,2,2,14],
 '_':[0,0,0,0,0,0,31], '=':[0,0,31,0,31,0,0],
}

def rows(c):
    return GLYPHS.get(c.upper(), GLYPHS['?'])

def atlas(scale, slot):
    chars = [chr(i) for i in range(32,127)]
    data = bytearray(struct.pack('<IHH8B',0x41464344,3,len(chars),6*scale,8*scale,7*scale,9*scale,slot,0,1,0))
    for i,c in enumerate(chars):
        data += struct.pack('<IHBB',ord(c),i,6*scale,0)
    for c in chars:
        for y in range(8*scale):
            for x in range(6*scale):
                on = y//scale < 7 and x//scale < 5 and rows(c)[y//scale] & (1 << (4-x//scale))
                data.append(255 if on else 0)
    return data

def generate(out):
    out.mkdir(parents=True,exist_ok=True)
    arrays={'font_small':atlas(1,0),'font_large':atlas(2,1),
            'font_rows':bytes(v for i in range(32,127) for v in rows(chr(i)))}
    text='#pragma once\n#include <stdint.h>\n'
    for name,data in arrays.items():
        text += f'static const uint8_t {name}[] = {{\n'
        text += ',\n'.join(','.join(str(v) for v in data[i:i+32]) for i in range(0,len(data),32))
        text += '\n};\n'
    (out/'fonts.h').write_text(text,encoding='utf-8')

if __name__=='__main__':
    generate(Path(sys.argv[1]))
