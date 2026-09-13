#include "ds_frost.h"
#include <string.h>
_Static_assert(sizeof(ds_frost)==DS_FROST_BYTES,"frost budget");
static inline __attribute__((always_inline)) unsigned channel(uint16_t p,unsigned c){
    unsigned v=c==0?p>>11:c==1?(p>>5)&63:p&31;
    return c==1?(v<<2)|(v>>4):(v<<3)|(v>>2);
}
static uint16_t pack(const unsigned c[3]){return (uint16_t)((c[0]>>3)<<11|(c[1]>>2)<<5|(c[2]>>3));}
static int clamp(int v,int max){return v<0?0:v>max?max:v;}
void ds_frost_init(ds_frost *frost){if(frost)memset(frost,0,sizeof(*frost));}
ds_result ds_frost_feed(ds_frost *frost,uint16_t y,uint16_t rows,const uint16_t *pixels){
    if(!frost||!pixels||y>=135||y%8||rows!=(y==128?7:8))return DS_INVALID;
    if(frost->state.phase||y!=frost->state.next_y)return DS_STALE;
    for(unsigned col=0;col<30;col++){
        unsigned sum[3]={0},n=8u*rows;
        for(unsigned py=0;py<rows;py++)for(unsigned x=0;x<8;x++){
            uint16_t p=pixels[py*240+col*8+x];
            sum[0]+=channel(p,0);sum[1]+=channel(p,1);sum[2]+=channel(p,2);
        }
        for(unsigned c=0;c<3;c++)sum[c]=(sum[c]+n/2)/n;
        frost->state.image[(y/8)*30+col]=pack(sum);
    }
    frost->state.next_y=(uint16_t)(y+rows);return DS_OK;
}
ds_result ds_frost_blur(ds_frost *frost,uint8_t radius){
    if(!frost||(radius!=1&&radius!=2))return DS_INVALID;
    if(frost->state.phase||frost->state.next_y!=135)return DS_STALE;
    unsigned n=2u*radius+1;
    for(unsigned axis=0;axis<2;axis++){
        int length=axis?17:30,lines=axis?30:17;
        for(int line=0;line<lines;line++){
            for(int i=0;i<length;i++)frost->state.line[i]=frost->state.image[axis?i*30+line:line*30+i];
            unsigned sum[3]={0};
            for(int k=-(int)radius;k<=(int)radius;k++)
                for(unsigned c=0;c<3;c++)sum[c]+=channel(frost->state.line[clamp(k,length-1)],c);
            for(int i=0;i<length;i++){
                unsigned average[3];
                for(unsigned c=0;c<3;c++)average[c]=(sum[c]+n/2)/n;
                frost->state.image[axis?i*30+line:line*30+i]=pack(average);
                /* Preserve each pass's RGB565 rounding and replicated edges. */
                uint16_t leaving=frost->state.line[clamp(i-(int)radius,length-1)];
                uint16_t entering=frost->state.line[clamp(i+(int)radius+1,length-1)];
                for(unsigned c=0;c<3;c++)sum[c]=sum[c]-channel(leaving,c)+channel(entering,c);
            }
        }
    }
    frost->state.phase=1;return DS_OK;
}
ds_result ds_frost_span(const ds_frost *frost,uint16_t y,uint16_t x,uint16_t count,
                        ds_rgba tint,uint16_t *pixels){
    if(!frost||y>=135||x>240||count>240-x||(count&&!pixels))return DS_INVALID;
    if(frost->state.phase!=1)return DS_STALE;
    /* Pixel centers: (x+.5)/8-.5, in sixteenths; clamp before division. */
    int sy=clamp(2*(int)y-7,16*16),y0=sy/16,y1=clamp(y0+1,16),wy=sy%16;
    unsigned a=tint&255,ia=255-a,tinted[3];
    for(unsigned k=0;k<3;k++)tinted[k]=((tint>>(24-8*k))&255)*a+127;
    /* Two live columns keep the snapshot const and avoid extra quantization. */
    unsigned left[3],right[3];
    int previous=-1;
    for(unsigned i=0;i<count;i++){
        int sx=clamp(2*((int)x+(int)i)-7,29*16),x0=sx/16,x1=clamp(x0+1,29),wx=sx%16;
        if(x0!=previous){
            for(unsigned k=0;k<3;k++){
                left[k]=previous>=0&&previous==x0-1?right[k]:
                    channel(frost->state.image[y0*30+x0],k)*(16-wy)+channel(frost->state.image[y1*30+x0],k)*wy;
                right[k]=channel(frost->state.image[y0*30+x1],k)*(16-wy)+channel(frost->state.image[y1*30+x1],k)*wy;
            }
            previous=x0;
        }
        unsigned c[3];
        for(unsigned k=0;k<3;k++){
            unsigned value=(left[k]*(16-wx)+right[k]*wx+128)/256;
            c[k]=(tinted[k]+value*ia)/255;
        }
        pixels[i]=pack(c);
    }
    return DS_OK;
}
