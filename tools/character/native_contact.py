"""Contact sheet from actual QuickJS/native Kasane PPM captures, not reference art."""
import argparse
from pathlib import Path
import struct
import zlib


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('folder',type=Path)
    parser.add_argument('--output',type=Path)
    args = parser.parse_args()
    offsets = [0,90,171,301+90,602+90]
    w,h,cols,rows=240,135,5,3
    raw=bytearray((w*cols*3+1)*h*rows)
    for model in range(rows):
        for col,offset in enumerate(offsets):
            file=args.folder/f'app_{model*1806+offset:04d}.ppm'
            content=file.read_bytes();header=b'P6\n240 135\n255\n'
            if not content.startswith(header) or len(content)!=len(header)+w*h*3:
                raise ValueError(f'invalid native capture {file}')
            image=content[len(header):]
            for y in range(h):
                start=(model*h+y)*(w*cols*3+1)+1+col*w*3
                raw[start:start+w*3]=image[y*w*3:(y+1)*w*3]
    def chunk(tag,data):
        return struct.pack('>I',len(data))+tag+data+struct.pack('>I',zlib.crc32(tag+data))
    output=args.output or args.folder/'contact.png'
    output.write_bytes(b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',w*cols,h*rows,8,2,0,0,0))+
                       chunk(b'IDAT',zlib.compress(raw,9))+chunk(b'IEND',b''))
    print(f'Wrote {output}; columns neutral, body tilt, shape stretch, light only, camera only')


if __name__=='__main__':
    main()
