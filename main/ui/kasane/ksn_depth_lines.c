#include "ksn_depth_lines.h"
#include <stdlib.h>
#include <string.h>

void ksn_depth_frame_reset(ksn_depth_frame *frame){
    if(frame){frame->count=0;frame->raster_steps=0;}
}
bool ksn_depth_add(ksn_depth_frame *frame,ksn_depth_line line){
    if(!frame||frame->count>=KSN_DEPTH_LINES_MAX||
       line.x0< -480||line.x0>720||line.x1< -480||line.x1>720||
       line.y0< -480||line.y0>720||line.y1< -480||line.y1>720||
       line.z0==UINT16_MAX||line.z1==UINT16_MAX)return false;
    unsigned dx=(unsigned)abs(line.x1-line.x0),dy=(unsigned)abs(line.y1-line.y0);
    unsigned cost=(dx>dy?dx:dy)+1u;
    if(cost>KSN_DEPTH_RASTER_MAX-frame->raster_steps)return false;
    frame->lines[frame->count++]=line;
    frame->raster_steps=(uint16_t)(frame->raster_steps+cost);
    return true;
}
bool ksn_depth_image_init(ksn_depth_image *image,const ksn_depth_frame *frame,
                          uint16_t width,uint16_t height){
    if(!image||!frame||!width||width>240||!height||height>135)return false;
    memset(image,0,sizeof *image);
    image->frame=frame;image->width=width;image->height=height;
    return true;
}
void ksn_depth_image_bind(ksn_depth_image *image,const ksn_depth_frame *frame){
    if(image){image->frame=frame;image->cached=false;}
}
static void render_row(ksn_depth_image *image,uint16_t row){
    for(unsigned x=0;x<image->width;x++){
        image->depth[x]=UINT16_MAX;
        image->color[x]=0;
        image->alpha[x]=0;
    }
    for(unsigned i=0;i<image->frame->count;i++){
        const ksn_depth_line *line=&image->frame->lines[i];
        if((line->y0<line->y1?line->y1:line->y0)<row||
           (line->y0<line->y1?line->y0:line->y1)>row)continue;
        int x=line->x0,y=line->y0;
        int dx=abs(line->x1-line->x0),dy=abs(line->y1-line->y0);
        int sx=x<line->x1?1:-1,sy=y<line->y1?1:-1,err=dx-dy;
        unsigned steps=(unsigned)(dx>dy?dx:dy);
        for(unsigned at=0;;at++){
            if(y==row&&x>=0&&x<image->width){
                uint16_t z=steps?(uint16_t)(((uint32_t)(steps-at)*line->z0+
                                      (uint32_t)at*line->z1+steps/2u)/steps):line->z0;
                if(z<image->depth[x]){
                    image->depth[x]=z;
                    image->color[x]=line->color;
                    image->alpha[x]=255;
                }
            }
            if(x==line->x1&&y==line->y1)break;
            int twice=2*err;
            if(twice> -dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;y+=sy;}
        }
    }
    image->cached_y=row;image->cached=true;
}
static ksn_result read_span(void *ctx,uint16_t variant,uint16_t frame,
                            uint16_t y,uint16_t x,uint16_t count,
                            uint16_t *rgb565,uint8_t *alpha){
    ksn_depth_image *image=ctx;
    if(!image||!image->frame||variant||frame||y>=image->height||
       x>image->width||count>image->width-x||
       (count&&(!rgb565||!alpha)))return KSN_INVALID;
    if(count){
        if(!image->cached||image->cached_y!=y)render_row(image,y);
        memcpy(rgb565,image->color+x,(size_t)count*sizeof *rgb565);
        memcpy(alpha,image->alpha+x,count);
    }
    return KSN_OK;
}
void ksn_depth_image_port(ksn_depth_image *image,ksn_image_port *out){
    if(!out)return;
    *out=image&&image->frame?(ksn_image_port){
        .ctx=image,.width=image->width,.height=image->height,
        .variants=1,.frames=1,.read_span=read_span,.opaque=false
    }:(ksn_image_port){0};
}
