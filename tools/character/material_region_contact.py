"""Label renderer evidence and encode a one-shot host yaw sweep; no device FPS."""
import argparse
import ast
from pathlib import Path
import struct
import zlib
from fox_contact import write_panels

ROOT=Path(__file__).resolve().parents[2]
def chunk(tag,data):
    return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('folder',type=Path);args=p.parse_args()
    names=['SINGLE COLOR','EXISTING UV BAND','LOCAL PLANE HARD','LOCAL PLANE FEATHER','LIGHT ONLY','POSE ROTATION','NONUNIFORM SCALE','SPEC ONLY - OLD','SPEC ONLY - REGION']
    print(write_panels(args.folder,list(enumerate(names)),'HOST_SIMULATION_material-region.png',
                      'HOST SIMULATION / MATERIAL REGION / NOT DEVICE CAPTURE / PIXELS ENLARGED 2X'))
    table=next(n.value for n in ast.parse((ROOT/'tools/make_font.py').read_text()).body
               if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='GLYPHS' for t in n.targets))
    glyphs={ast.literal_eval(k):([0]*7 if ast.literal_eval(k)==' ' else ast.literal_eval(v)) for k,v in zip(table.keys,table.values)}
    width,height=480,288
    def frame(index):
        raw=(args.folder/f'app_{index:04}.ppm').read_bytes();header=b'P6\n240 135\n255\n'
        if not raw.startswith(header) or len(raw)!=len(header)+240*135*3:raise ValueError('incomplete renderer capture')
        pixels=raw[len(header):];rgb=bytearray(bytes([28,32,37])*width*height)
        for y,line in enumerate(['HOST SIMULATION / 30 SAMPLES/S / NOT DEVICE FPS','BODY YAW -35 TO +35 / FIXED WORLD LIGHT']):
            for i,c in enumerate(line):
                for gy,row in enumerate(glyphs.get(c,glyphs['?'])):
                    for gx in range(5):
                        if row&(16>>gx):
                            at=((1+y*9+gy)*width+4+i*6+gx)*3;rgb[at:at+3]=bytes([255,215,170])
        for sy in range(135):
            row=b''.join(pixels[(sy*240+x)*3:(sy*240+x+1)*3]*2 for x in range(240))
            for dy in range(2):
                at=((18+sy*2+dy)*width)*3;rgb[at:at+width*3]=row
        return zlib.compress(b''.join(b'\0'+rgb[y*width*3:(y+1)*width*3] for y in range(height)),9)
    output=bytearray(b'\x89PNG\r\n\x1a\n')+chunk(b'IHDR',struct.pack('>IIBBBBB',width,height,8,2,0,0,0))
    output+=chunk(b'tEXt',b'Description\0HOST SIMULATION; one-shot yaw sweep; 30 samples/s, not device FPS')
    output+=chunk(b'acTL',struct.pack('>II',61,1));sequence=0
    for i in range(61):
        output+=chunk(b'fcTL',struct.pack('>IIIIIHHBB',sequence,width,height,0,0,1,30,0,0));sequence+=1
        payload=frame(9+i)
        if i==0:output+=chunk(b'IDAT',payload)
        else:output+=chunk(b'fdAT',struct.pack('>I',sequence)+payload);sequence+=1
    output+=chunk(b'IEND',b'');path=args.folder/'HOST_SIMULATION_material-region-motion.png';path.write_bytes(output);print(path)

if __name__=='__main__':main()
