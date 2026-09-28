#include "ksn_pixel_span.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint32_t seed=0x9e3779b9u;
static uint32_t next(void){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
static ksn_pixel_instruction op(unsigned code,unsigned dst,unsigned a,
                                unsigned b,unsigned imm){
    return (ksn_pixel_instruction){(uint8_t)code,(uint8_t)dst,(uint8_t)a,
                                   (uint8_t)b,(uint16_t)imm};
}

static void equivalence(void){
    uint16_t source[240*135];
    for(unsigned i=0;i<240u*135u;i++)source[i]=(uint16_t)next();
    for(unsigned trial=0;trial<180;trial++){
        unsigned width=1u+next()%240u,height=1u+next()%135u;
        unsigned count=6u+next()%19u;
        while(width*height*count>KSN_PIXEL_WORK_MAX)count--;
        ksn_pixel_frame frame={.width=(uint16_t)width,.height=(uint16_t)height,
                               .count=(uint8_t)count,.alpha_reg=3};
        frame.params[0]=(uint16_t)(next()&511u);
        frame.code[0]=op(KSN_PIXEL_X,0,255,255,0);
        frame.code[1]=op(KSN_PIXEL_Y,1,255,255,0);
        frame.code[2]=op(KSN_PIXEL_IMM,2,255,255,next());
        frame.code[3]=op(KSN_PIXEL_PARAM,3,255,255,0);
        frame.code[4]=op(trial&1u?KSN_PIXEL_UNDERLAY:KSN_PIXEL_IMM,
                         4,255,255,next());
        static const unsigned ops[]={KSN_PIXEL_ADD,KSN_PIXEL_SUB,
            KSN_PIXEL_MUL,KSN_PIXEL_SHR,KSN_PIXEL_AND,KSN_PIXEL_OR,KSN_PIXEL_XOR};
        for(unsigned i=5;i<count;i++){
            unsigned kind=ops[next()%7u],a=next()%5u,b=next()%5u;
            frame.code[i]=op(kind,i%KSN_PIXEL_REGS,a,b,
                             kind==KSN_PIXEL_SHR?next()%16u:0);
        }
        frame.color_reg=(uint8_t)((count-1u)%KSN_PIXEL_REGS);
        ksn_pixel_underlay underlay={source,240u*135u,240};
        ksn_pixel_image image={0};ksn_image_port port;
        assert(ksn_pixel_image_bind(&image,&frame,&underlay));
        ksn_pixel_image_port(&image,&port);
        uint16_t old_color[240],new_color[240];
        uint8_t old_alpha[240],new_alpha[240];
        for(unsigned probe=0;probe<30;probe++){
            unsigned y=next()%height,x=next()%width;
            unsigned length=1u+next()%(width-x);
            assert(port.read_span(port.ctx,0,0,(uint16_t)y,(uint16_t)x,
                                  (uint16_t)length,old_color,old_alpha)==KSN_OK);
            assert(ksn_pixel_span_eval(&image,(uint16_t)y,(uint16_t)x,
                                       (uint16_t)length,new_color,new_alpha));
            assert(!memcmp(old_color,new_color,length*sizeof(uint16_t)));
            assert(!memcmp(old_alpha,new_alpha,length));
        }
    }
    puts("pixel span: 180 valid programs x 30 reordered spans matched");
}

static void high_register_aliases(void){
    uint16_t source[31*19];
    for(unsigned i=0;i<31u*19u;i++)source[i]=(uint16_t)next();
    ksn_pixel_underlay underlay={source,31u*19u,31};
    ksn_pixel_frame frame={.width=31,.height=19,.count=12,
                           .color_reg=7,.alpha_reg=6};
    frame.params[0]=73;
    frame.code[0]=op(KSN_PIXEL_X,5,255,255,0);
    frame.code[1]=op(KSN_PIXEL_Y,6,255,255,0);
    frame.code[2]=op(KSN_PIXEL_PARAM,7,255,255,0);
    frame.code[3]=op(KSN_PIXEL_ADD,5,5,7,0); /* dst aliases a */
    frame.code[4]=op(KSN_PIXEL_MUL,6,6,5,0); /* dst aliases a */
    frame.code[5]=op(KSN_PIXEL_SHR,7,6,255,3);
    frame.code[6]=op(KSN_PIXEL_XOR,7,7,5,0); /* dst aliases a */
    frame.code[7]=op(KSN_PIXEL_AND,6,7,6,0); /* dst aliases b */
    frame.code[8]=op(KSN_PIXEL_OR,5,6,7,0);
    frame.code[9]=op(KSN_PIXEL_UNDERLAY,6,255,255,0);
    frame.code[10]=op(KSN_PIXEL_ADD,7,7,6,0); /* high register input */
    frame.code[11]=op(KSN_PIXEL_MUL,6,6,6,0); /* dst aliases both */
    ksn_pixel_image image={0};ksn_image_port port;
    assert(ksn_pixel_image_bind(&image,&frame,&underlay));
    ksn_pixel_image_port(&image,&port);
    uint16_t old_color[31],new_color[31];
    uint8_t old_alpha[31],new_alpha[31];
    for(int y=18;y>=0;y--)for(unsigned x=0;x<31;x+=3){
        unsigned count=31-x;
        assert(port.read_span(port.ctx,0,0,(uint16_t)y,(uint16_t)x,
                              (uint16_t)count,old_color,old_alpha)==KSN_OK);
        assert(ksn_pixel_span_eval(&image,(uint16_t)y,(uint16_t)x,
                                   (uint16_t)count,new_color,new_alpha));
        assert(!memcmp(old_color,new_color,count*sizeof(uint16_t)));
        assert(!memcmp(old_alpha,new_alpha,count));
    }
    puts("pixel span: registers 5..7, underlay, dst=a/b/both matched");
}

static void repair_probe_samples(void){
    ksn_pixel_frame frame={.width=112,.height=63,.count=8,
                           .color_reg=6,.alpha_reg=4};
    frame.code[0]=op(KSN_PIXEL_X,0,0,0,0);
    frame.code[1]=op(KSN_PIXEL_Y,1,0,0,0);
    frame.code[2]=op(KSN_PIXEL_PARAM,2,0,0,0);
    frame.code[3]=op(KSN_PIXEL_ADD,0,0,2,0);
    frame.code[4]=op(KSN_PIXEL_MUL,3,0,1,0);
    frame.code[5]=op(KSN_PIXEL_SHR,4,3,0,2);
    frame.code[6]=op(KSN_PIXEL_XOR,5,4,2,0);
    frame.code[7]=op(KSN_PIXEL_AND,6,5,0,0);
    const uint16_t expected[]={0x00a0,0x009c};
    for(unsigned i=0;i<2;i++){
        frame.params[0]=(uint16_t)(((121u+i)*29u+17u)&1023u);
        ksn_pixel_image image={0};ksn_image_port port;
        assert(ksn_pixel_image_bind(&image,&frame,NULL));
        ksn_pixel_image_port(&image,&port);
        uint16_t old_color=0,new_color=0;
        uint8_t old_alpha=0,new_alpha=0;
        assert(port.read_span(port.ctx,0,0,3,26,1,&old_color,&old_alpha)==KSN_OK);
        assert(ksn_pixel_span_eval(&image,3,26,1,&new_color,&new_alpha));
        assert(old_color==expected[i]&&new_color==expected[i]);
        assert(old_alpha==255&&new_alpha==255);
    }
    puts("pixel span: repair first-band samples 00a0/009c matched");
}

static void benchmark(void){
    ksn_pixel_frame frame={.width=240,.height=135,.count=18,
                           .color_reg=0,.alpha_reg=1};
    frame.code[0]=op(KSN_PIXEL_X,0,255,255,0);
    frame.code[1]=op(KSN_PIXEL_IMM,1,255,255,255);
    for(unsigned i=2;i<18;i++)frame.code[i]=op(KSN_PIXEL_ADD,0,0,0,0);
    ksn_pixel_image image={0};ksn_image_port port;
    assert(ksn_pixel_image_bind(&image,&frame,NULL));
    ksn_pixel_image_port(&image,&port);
    uint16_t color[240];uint8_t alpha[240];
    unsigned long sums[2]={0};double ms[2]={0};
    for(unsigned mode=0;mode<2;mode++){
        clock_t started=clock();
        for(unsigned repeat=0;repeat<64;repeat++)for(unsigned y=0;y<135;y++){
            if(mode)assert(ksn_pixel_span_eval(&image,(uint16_t)y,0,240,color,alpha));
            else assert(port.read_span(port.ctx,0,0,(uint16_t)y,0,240,
                                       color,alpha)==KSN_OK);
            sums[mode]+=color[239]+alpha[239];
        }
        ms[mode]=1000.0*(double)(clock()-started)/CLOCKS_PER_SEC/64.0;
    }
    assert(sums[0]==sums[1]);
    printf("pixel span host: pixel_switch=%.3fms instruction_major=%.3fms checksum=%lu\n",
           ms[0],ms[1],sums[0]);
}

int main(void){equivalence();high_register_aliases();repair_probe_samples();benchmark();return 0;}
