"""Label the twelve native host renders, preserving nearest-neighbour pixels."""
import ast
from pathlib import Path
import struct
import zlib


def write_contact(folder):
    panels=[(e*3+k,f'{name} YAW {yaw:+d} PITCH 0')
            for e,name in enumerate(('NEUTRAL','SMILE','SURPRISE','JAW OPEN')) for k,yaw in enumerate((-35,0,35))]
    return write_panels(folder,panels,'HOST_SIMULATION_fox_contact.png')


def write_aux_contact(folder):
    panels=[(12+j*3+k,f'NEUTRAL YAW {yaw:+d} PITCH {pitch:+d}')
            for j,pitch in enumerate((-10,10)) for k,yaw in enumerate((-35,0,35))]
    panels += [(36+j*3+k,f'NEUTRAL YAW {yaw:+d} LIGHT {light:+d}')
               for j,light in enumerate((-150,150)) for k,yaw in enumerate((-35,0,35))]
    return write_panels(folder,panels,'HOST_SIMULATION_fox_pitch_backlight.png')


def write_panels(folder,panels,filename,title=None):
    root=Path(__file__).resolve().parents[2]
    table=next(n.value for n in ast.parse((root/'tools/make_font.py').read_text()).body
               if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='GLYPHS' for t in n.targets))
    glyphs={ast.literal_eval(k):([0]*7 if ast.literal_eval(k)==' ' else ast.literal_eval(v)) for k,v in zip(table.keys,table.values)}
    glyphs[' ']=[0]*7
    width,height=1440,1228
    rgb=bytearray(bytes([28,32,37])*(width*height))
    def text(x,y,value):
        for c in value.upper():
            for gy,row in enumerate(glyphs.get(c,glyphs['?'])):
                for gx in range(5):
                    if row&(16>>gx):
                        for dy in range(2):
                            for dx in range(2):
                                at=((y+gy*2+dy)*width+x+gx*2+dx)*3;rgb[at:at+3]=bytes([255,215,170])
            x+=12
    text(10,5,title or 'HOST SIMULATION / FOX 3D FACE SLICE / NOT DEVICE CAPTURE / PIXELS ENLARGED 2X')
    for panel,(frame,label) in enumerate(panels):
            file=folder/f'app_{frame:04}.ppm';data=file.read_bytes();header=b'P6\n240 135\n255\n'
            if not data.startswith(header) or len(data)!=len(header)+240*135*3:raise ValueError(str(file))
            pixels=data[len(header):];x=(panel%3)*480;y=28+(panel//3)*300;text(x+8,y+3,label)
            for sy in range(135):
                row=b''.join(pixels[(sy*240+sx)*3:(sy*240+sx+1)*3]*2 for sx in range(240))
                for dy in range(2):
                    at=((y+25+sy*2+dy)*width+x)*3;rgb[at:at+1440]=row
    def chunk(tag,data):return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
    scan=b''.join(b'\0'+rgb[y*width*3:(y+1)*width*3] for y in range(height))
    output=folder/filename
    output.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))+
                       chunk(b'tEXt',b'Description\0HOST SIMULATION; not device capture; nearest enlargement')+
                       chunk(b'IDAT',zlib.compress(scan,9))+chunk(b'IEND',b''))
    return output
