#include "ksn_font.h"
#include "jpfont.h"
#include "utf8.h"
#include "fonts.h"
#include <string.h>

static ksn_result span(void *ctx,const ksn_draw *draw,uint16_t reveal,int x,int y,
                        unsigned count,uint8_t *out){
    (void)ctx;
    if(!draw||draw->kind!=KSN_TEXT||(unsigned)draw->data.text.font>KSN_DISPLAY||count>64||
       (count&&!out))return KSN_INVALID;
    if(!count)return KSN_OK;
    memset(out,0,count);
    ksn_font font=draw->data.text.font;
    unsigned scale=font==KSN_DISPLAY?2:1;
    unsigned line_height=font==KSN_BODY?12:8*scale;
    int gy=y-draw->bounds.y0;
    if(gy<0||gy>=(int)line_height)return KSN_OK;
    int pen=draw->bounds.x0;
    const char *s=draw->data.text.utf8;
    size_t bytes=draw->data.text.bytes;
    for(size_t at=0;at<bytes&&reveal&&pen<x+(int)count;reveal--){
        size_t consumed;
        uint32_t cp=utf8_decode(s,bytes,at,&consumed);at+=consumed;
        jpfont_bitmap_view glyph={0};
        bool mapped=(font==KSN_BODY||cp>=128)&&
            jpfont_bitmap(font==KSN_BODY?JPFONT_TEXT:JPFONT_SMALL,cp,&glyph);
        // Logical metrics do not change when a glyph/face is missing.
        unsigned advance=(cp<128?6:font==KSN_BODY?12:8)*scale;
        unsigned width=mapped?glyph.width*scale:advance;
        if(width>advance)width=advance;
        for(unsigned i=0;i<count;i++){
            int gx=x+(int)i-pen;
            if(gx<0||gx>=(int)width)continue;
            bool ink=false;
            if(mapped){
                unsigned row=(unsigned)gy/scale,col=(unsigned)gx/scale;
                if(row<glyph.height)ink=(glyph.bits[row*glyph.stride+col/8]&(0x80u>>(col&7)))!=0;
            }else if(cp<128){
                unsigned row=(unsigned)gy/scale,col=(unsigned)gx/scale;
                if(row<7&&col<5){unsigned ch=cp>=32&&cp<=126?cp:'?';
                    ink=(font_rows[(ch-32)*7+row]&(1u<<(4-col)))!=0;}
            }else{
                // Missing face: a same-advance tofu, not an invisible glyph.
                ink=gx<(int)advance-1&&gy<(int)line_height-1&&
                    (gx==0||gx==(int)advance-2||gy==0||gy==(int)line_height-2);
            }
            if(ink)out[i]=255;
        }
        pen+=(int)advance;
    }
    return KSN_OK;
}
const ksn_text_port ksn_font_port={.span=span};
