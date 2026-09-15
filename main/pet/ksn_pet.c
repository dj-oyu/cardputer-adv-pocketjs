#include "ksn_pet.h"
#include "pet_pixels.h"

static ksn_result read_span(void *ctx,uint16_t pet,uint16_t mood,uint16_t y,uint16_t x,
                            uint16_t count,uint16_t *rgb,uint8_t *alpha){
    if(!ctx||pet>=12||mood>=6||y>=64||x>64||count>64-x||
       (count&&(!rgb||!alpha)))return KSN_INVALID;
    if(!count)return KSN_OK;
    uint16_t row[64]; // 128-byte provider row, included in compositor scratch.
    pet_pixels_row(ctx,pet,y,row);pet_pixels_face(pet,y,mood,row);
    for(unsigned i=0;i<count;i++){
        unsigned v=row[x+i],r=v&15,g=(v>>4)&15,b=(v>>8)&15;
        rgb[i]=(uint16_t)(((r<<1|r>>3)<<11)|((g<<2|g>>2)<<5)|(b<<1|b>>3));
        alpha[i]=(uint8_t)((v>>12)*17);
    }
    return KSN_OK;
}
ksn_result ksn_pet_image(const uint8_t *data,size_t bytes,ksn_image_port *out){
    if(!out||!pet_pixels_valid(data,bytes))return KSN_INVALID;
    *out=(ksn_image_port){(void *)data,64,64,12,6,read_span};return KSN_OK;
}
