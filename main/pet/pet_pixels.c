#include "pet_pixels.h"
#include <string.h>
#include <stdlib.h>

enum { ROWS=4, PALETTE=390, OFFSETS=774, BODY=800 };
static unsigned u16(const uint8_t *p) { return p[0] | (unsigned)p[1]<<8; }

bool pet_pixels_valid(const uint8_t *d, size_t n) {
    if(!d || n<BODY || memcmp(d,"PPT2",4)) return false;
    unsigned patches=BODY+u16(d+ROWS+192*2);
    if(patches>n || (u16(d+ROWS)&32767)) return false;
    for(unsigned row=0;row<192;row++) {
        unsigned start=u16(d+ROWS+row*2), end=u16(d+ROWS+(row+1)*2)&32767;
        unsigned begin=start&32767;
        if(end<begin || BODY+end>patches) return false;
        if(start&32768) { if(end-begin!=32) return false; }
        else {
            unsigned count=0;
            for(unsigned off=begin;off<end;off++) count+=(d[BODY+off]>>4)+1;
            if(count!=64) return false;
        }
    }
    unsigned last=0;
    for(unsigned pet=0;pet<=12;pet++) {
        unsigned end=u16(d+OFFSETS+pet*2);
        if(end<last || end%2 || patches+end>n || (!pet && end)) return false;
        unsigned previous=0;
        for(unsigned off=last;off<end;off+=2) {
            unsigned v=u16(d+patches+off), pos=v&4095;
            if(!(v>>12) || (off!=last && pos<=previous)) return false;
            previous=pos;
        }
        last=end;
    }
    return patches+last==n;
}

void pet_pixels_row(const uint8_t *d, unsigned pet, unsigned y, uint16_t row[64]) {
    if(pet>=12 || y>=64) { memset(row,0,128); return; }
    unsigned start=u16(d+ROWS+((pet/4)*64+y)*2);
    const uint8_t *base=d+BODY+(start&32767);
    const uint8_t *palette=d+PALETTE+pet*32;
    if(start&32768) {
        for(unsigned x=0;x<64;x++) row[x]=(uint16_t)u16(palette+((base[x/2]>>(x%2*4))&15)*2);
    } else {
        for(unsigned x=0;x<64;) {
            unsigned v=*base++,count=(v>>4)+1;
            uint16_t color=(uint16_t)u16(palette+(v&15)*2);
            while(count--) row[x++]=color;
        }
    }
    unsigned patches=BODY+u16(d+ROWS+192*2);
    unsigned lo=u16(d+OFFSETS+pet*2)/2, hi=u16(d+OFFSETS+(pet+1)*2)/2, end=hi;
    // Seek to this row instead of rescanning all markings for each strip.
    while(lo<hi) {
        unsigned mid=lo+(hi-lo)/2;
        if((u16(d+patches+mid*2)&4095)<y*64) lo=mid+1; else hi=mid;
    }
    for(;lo<end;lo++) {
        unsigned v=u16(d+patches+lo*2), pos=v&4095;
        if(pos>=(y+1)*64) break;
        row[pos%64]=(uint16_t)u16(palette+(v>>12)*2);
    }
}

void pet_pixels_face(unsigned pet,unsigned y,unsigned mood,uint16_t row[64]) {
    static const uint8_t eyes[3][3]={{16,32,24},{17,36,32},{16,29,27}};
    if(pet>=12 || y>=64) return;
    const uint8_t *e=eyes[pet/4];
    int dy=(int)y-e[2];
    unsigned ink=pet%4==3?0xffd1:pet==2?0xf3bf:0xf123;
    for(unsigned i=0;i<2;i++) for(int dx=-2;dx<=2;dx++) {
        bool on;
        if(mood==1 || (mood==5 && i==1)) on=dy==1;
        else if(mood==2) on=dy==abs(dx)-1;
        else if(mood==3) on=abs(dx)+abs(dy)<=3 && (abs(dx)==2 || abs(dy)==2);
        else if(mood==4) on=dy>=0 && dy<=1 && abs(dx)<=1;
        else on=abs(dx)+abs(dy)<=3;
        if(abs(dy)>2 || !on) continue;
        row[e[i]+dx]=(uint16_t)ink;
        if((mood==0 || (mood==5 && i==0)) && dx==-1 && dy==-1) row[e[i]+dx]=0xfeff;
        if(pet==2 && (mood==0 || (mood==5 && i==0)) && dx==0 && abs(dy)<=1) row[e[i]+dx]=0xf111;
    }
}

void pet_pixels_draw(const uint8_t *d, unsigned pet, uint16_t *strip,
                     int width, int strip_y, int rows, int x, int y, unsigned divisor,unsigned mood) {
    if(pet>=12 || (divisor!=1 && divisor!=2) || width<=0 || rows<=0) return;
    uint16_t row[64];
    int side=64/(int)divisor;
    for(int py=0;py<rows;py++) {
        int sy=strip_y+py-y;
        if(sy<0 || sy>=side) continue;
        pet_pixels_row(d,pet,(unsigned)sy*divisor,row);
        pet_pixels_face(pet,(unsigned)sy*divisor,mood,row);
        for(int px=0;px<side;px++) {
            if(x+px<0 || x+px>=width) continue;
            unsigned v=row[px*divisor];
            if(!(v>>12)) continue;
            unsigned r=(v&15)*17, g=((v>>4)&15)*17, b=((v>>8)&15)*17;
            strip[py*width+x+px]=(uint16_t)((r>>3)<<11 | (g>>2)<<5 | b>>3);
        }
    }
}
