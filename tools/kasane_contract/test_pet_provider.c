#include "ksn_pet.h"
#include "pet_pixels.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void){
    FILE *f=fopen("apps/pet/assets/pets-compact.bin","rb");assert(f);
    assert(!fseek(f,0,SEEK_END));long length=ftell(f);assert(length>800);rewind(f);
    uint8_t *data=malloc((size_t)length);assert(data);
    assert(fread(data,1,(size_t)length,f)==(size_t)length);fclose(f);
    ksn_image_port port={0},sentinel={0};
    assert(ksn_pet_image(data,(size_t)length-1,&port)==KSN_INVALID&&!memcmp(&port,&sentinel,sizeof(port)));
    assert(ksn_pet_image(data,(size_t)length,NULL)==KSN_INVALID);
    assert(ksn_pet_image(data,(size_t)length,&port)==KSN_OK);
    assert(port.width==64&&port.height==64&&port.variants==12&&port.frames==6&&port.ctx==data);
    uint16_t row[64],rgb[66];uint8_t alpha[66];
    for(unsigned pet=0;pet<12;pet++)for(unsigned mood=0;mood<6;mood++)for(unsigned y=0;y<64;y++){
        pet_pixels_row(data,pet,y,row);pet_pixels_face(pet,y,mood,row);
        for(unsigned x=0;x<64;x+=7){
            unsigned count=64-x;if(count>31)count=31;
            for(unsigned i=0;i<66;i++){rgb[i]=0xa55a;alpha[i]=0x5a;}
            assert(port.read_span(port.ctx,pet,mood,y,x,count,rgb+1,alpha+1)==KSN_OK);
            assert(rgb[0]==0xa55a&&alpha[0]==0x5a&&rgb[count+1]==0xa55a&&alpha[count+1]==0x5a);
            for(unsigned i=0;i<count;i++){
                unsigned v=row[x+i],r=(v&15)*17,g=((v>>4)&15)*17,b=((v>>8)&15)*17;
                assert(rgb[i+1]==((r>>3)<<11|(g>>2)<<5|(b>>3))&&alpha[i+1]==(v>>12)*17);
            }
        }
    }
    assert(port.read_span(port.ctx,12,0,0,0,1,rgb,alpha)==KSN_INVALID);
    assert(port.read_span(port.ctx,0,6,0,0,1,rgb,alpha)==KSN_INVALID);
    assert(port.read_span(port.ctx,0,0,64,0,1,rgb,alpha)==KSN_INVALID);
    assert(port.read_span(port.ctx,0,0,0,63,2,rgb,alpha)==KSN_INVALID);
    assert(port.read_span(port.ctx,0,0,0,0,1,NULL,alpha)==KSN_INVALID);
    assert(port.read_span(port.ctx,0,0,0,64,0,NULL,NULL)==KSN_OK);
    free(data);puts("PPT2 provider PASS: 12 pets x 6 moods, every row/span, RGB565+A8, guards, malformed image");
}
