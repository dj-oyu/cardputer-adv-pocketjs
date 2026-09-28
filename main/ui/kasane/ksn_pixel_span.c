#include "ksn_pixel_span.h"

bool ksn_pixel_span_eval(const ksn_pixel_image *image, uint16_t y, uint16_t x,
                         uint16_t count, uint16_t *rgb565, uint8_t *alpha){
    if(!image||!image->valid||y>=image->frame.height||
       x>image->frame.width||count>image->frame.width-x||
       (count&&(!rgb565||!alpha)))return false;
    if(!count)return true;
    const ksn_pixel_frame *f=&image->frame;
    uint16_t reg[KSN_PIXEL_REGS][240];
    for(unsigned i=0;i<f->count;i++){
        const ksn_pixel_instruction *ins=&f->code[i];
        uint16_t *dst=reg[ins->dst];
        /* Unary and source instructions do not validate unused a/b fields. */
        const uint16_t *a=reg[ins->a & 7u],*b=reg[ins->b & 7u];
        switch(ins->op){
        case KSN_PIXEL_IMM:
            for(unsigned lane=0;lane<count;lane++)dst[lane]=ins->immediate;
            break;
        case KSN_PIXEL_X:
            for(unsigned lane=0;lane<count;lane++)dst[lane]=(uint16_t)(x+lane);
            break;
        case KSN_PIXEL_Y:
            for(unsigned lane=0;lane<count;lane++)dst[lane]=y;
            break;
        case KSN_PIXEL_PARAM:
            for(unsigned lane=0;lane<count;lane++)dst[lane]=f->params[ins->immediate];
            break;
        case KSN_PIXEL_UNDERLAY:
            for(unsigned lane=0;lane<count;lane++)
                dst[lane]=image->underlay.rgb565[(size_t)y*image->underlay.stride+x+lane];
            break;
        case KSN_PIXEL_ADD:
            for(unsigned lane=0;lane<count;lane++)dst[lane]=(uint16_t)(a[lane]+b[lane]);
            break;
        case KSN_PIXEL_SUB:
            for(unsigned lane=0;lane<count;lane++)dst[lane]=(uint16_t)(a[lane]-b[lane]);
            break;
        case KSN_PIXEL_MUL:
            for(unsigned lane=0;lane<count;lane++)
                dst[lane]=(uint16_t)((uint32_t)a[lane]*b[lane]);
            break;
        case KSN_PIXEL_SHR:
            for(unsigned lane=0;lane<count;lane++)dst[lane]=(uint16_t)(a[lane]>>ins->immediate);
            break;
        case KSN_PIXEL_AND:
            for(unsigned lane=0;lane<count;lane++)dst[lane]=(uint16_t)(a[lane]&b[lane]);
            break;
        case KSN_PIXEL_OR:
            for(unsigned lane=0;lane<count;lane++)dst[lane]=(uint16_t)(a[lane]|b[lane]);
            break;
        case KSN_PIXEL_XOR:
            for(unsigned lane=0;lane<count;lane++)dst[lane]=(uint16_t)(a[lane]^b[lane]);
            break;
        default:return false; /* Bound frame has already been validated. */
        }
    }
    const uint16_t *colors=reg[f->color_reg],*alphas=reg[f->alpha_reg];
    for(unsigned lane=0;lane<count;lane++){
        rgb565[lane]=colors[lane];
        alpha[lane]=alphas[lane]>255?255:(uint8_t)alphas[lane];
    }
    return true;
}
