// Host-side oracle: compare the row/strip decoder against random pixel lookup.
#include "pet_pixels.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned read16(const uint8_t *p) { return p[0]+256u*p[1]; }
static uint16_t reference(const uint8_t *d,unsigned pet,unsigned x,unsigned y) {
    unsigned pos=y*64+x;
    unsigned start=read16(d+4+(pet/4*64+y)*2),idx=0;
    const uint8_t *base=d+800+(start&32767);
    if(start&32768) idx=(base[x/2]>>(x%2*4))&15;
    else { unsigned seen=0;do { unsigned v=*base++;idx=v&15;seen+=(v>>4)+1; } while(seen<=x); }
    unsigned patches=800+read16(d+388);
    for(unsigned o=read16(d+774+pet*2);o<read16(d+776+pet*2);o+=2) {
        unsigned v=read16(d+patches+o);
        if((v&4095)==pos) idx=v>>12;
    }
    return (uint16_t)read16(d+390+pet*32+idx*2);
}
static uint16_t rgb565(unsigned v) {
    unsigned r=(v&15)*17,g=((v>>4)&15)*17,b=((v>>8)&15)*17;
    return (uint16_t)((r>>3)<<11|(g>>2)<<5|b>>3);
}
int main(int argc,char **argv) {
    assert(argc==2);
    FILE *f=fopen(argv[1],"rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>800);rewind(f);
    uint8_t *data=malloc((size_t)n);assert(data);
    assert(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);
    assert(pet_pixels_valid(data,(size_t)n));
    assert(!pet_pixels_valid(data,(size_t)n-1));
    data[0]^=1;assert(!pet_pixels_valid(data,(size_t)n));data[0]^=1;
    uint16_t row[64];
    static uint16_t oracle[12][4096];
    for(unsigned pet=0;pet<12;pet++) for(unsigned y=0;y<64;y++) {
        pet_pixels_row(data,pet,y,row);
        for(unsigned x=0;x<64;x++) assert(row[x]==reference(data,pet,x,y));
        pet_pixels_face(pet,y,0,row);
        memcpy(oracle[pet]+y*64,row,sizeof(row));
    }
    for(unsigned pet=0;pet<12;pet++) for(unsigned mood=1;mood<6;mood++) {
        unsigned changes=0;
        for(unsigned y=0;y<64;y++) {
            pet_pixels_row(data,pet,y,row);pet_pixels_face(pet,y,mood,row);
            for(unsigned x=0;x<64;x++) changes+=row[x]!=oracle[pet][y*64+x];
        }
        assert(changes>0 && changes<=60);
    }
    pet_pixels_row(data,12,0,row);for(unsigned x=0;x<64;x++) assert(!row[x]);
    // Blink uses the closed-eye expression, on BOTH anchors (the bird's
    // anchors are both left of x=32, so screen halves are not a valid test).
    const unsigned eyes[3][3]={{16,32,24},{17,36,32},{16,29,27}};
    for(unsigned pet=0;pet<12;pet++) for(unsigned y=0;y<64;y++) {
        memset(row,0,sizeof(row));pet_pixels_face(pet,y,1,row);
        for(unsigned x=0;x<64;x++) {
            const unsigned *e=eyes[pet/4];
            bool closed=y==e[2]+1 && ((x+2>=e[0] && x<=e[0]+2) || (x+2>=e[1] && x<=e[1]+2));
            assert((row[x]!=0)==closed);
        }
    }
    pet_pixels_row(data,0,64,row);for(unsigned x=0;x<64;x++) assert(!row[x]);
    const int positions[][2]={{27,29},{20,32},{5,5},{-31,-20},{220,120},{240,135}};
    uint16_t guarded[240*8+2];
    for(unsigned pet=0;pet<12;pet++) for(unsigned div=1;div<=2;div++)
    for(unsigned p=0;p<sizeof(positions)/sizeof(positions[0]);p++)
    for(int y=0;y<135;y+=8) {
        int rows=135-y<8?135-y:8;
        for(unsigned k=0;k<sizeof(guarded)/sizeof(guarded[0]);k++) guarded[k]=0xa55a;
        pet_pixels_draw(data,pet,guarded+1,240,y,rows,positions[p][0],positions[p][1],div,0);
        assert(guarded[0]==0xa55a && guarded[240*8+1]==0xa55a);
        for(int py=0;py<8;py++) for(int x=0;x<240;x++) {
            int sx=x-positions[p][0],sy=y+py-positions[p][1];
            uint16_t expected=0xa55a;
            if(py<rows && sx>=0 && sy>=0 && sx<64/(int)div && sy<64/(int)div) {
                unsigned v=oracle[pet][(unsigned)sy*div*64+(unsigned)sx*div];
                if(v>>12) expected=rgb565(v);
            }
            assert(guarded[1+py*240+x]==expected);
        }
    }
    free(data);
    puts("PASS: 12 pets, every pixel, 2 scales, clipping, guards, invalid data");
    return 0;
}
