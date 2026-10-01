/* Draws crowd_p24.mjs's pictures with the firmware's own VM and band
 * renderer (main/ui/kasane/ksn_procedural.c). Host only.
 *
 *   crowd_p24_host OUT.rgb565 < cases.txt
 *
 * cases.txt:
 *   FRAME back         a 240 x 135 picture cleared to RGB565 `back`
 *   D n                a draw: n rows "op dst a b value color", then
 *                      "I v0 .. v7"
 *   ENDFRAME
 * Each draw runs as pocket_proc.c runs it (a registered plan, then the
 * frame replayed in 8-row bands) over the same pixels, in order. OUT gets
 * every picture, 240 x 135 uint16 little endian; stdout one line a picture:
 * draws, steps, frame entries, raster steps (summed over its draws). */
#include "ksn_proc_plan.h"
#include <stdio.h>
#include <string.h>

static ksn_proc_frame frame;
static ksn_proc_vm vm;
static ksn_proc_plan plan;
static uint16_t pixels[KSN_PROC_W*KSN_PROC_H];

int main(int argc,char **argv){
    if(argc!=2){fprintf(stderr,"usage: crowd_p24_host OUT < cases\n");return 2;}
    FILE *out=fopen(argv[1],"wb");
    if(!out)return 2;
    char word[32];
    unsigned pictures=0;
    while(scanf("%31s",word)==1){
        if(strcmp(word,"FRAME")){fprintf(stderr,"expected FRAME, got %s\n",word);return 2;}
        unsigned back;
        if(scanf("%u",&back)!=1)return 2;
        for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)pixels[i]=(uint16_t)back;
        unsigned long draws=0,steps=0,entries=0,raster=0;
        for(;;){
            if(scanf("%31s",word)!=1)return 2;
            if(!strcmp(word,"ENDFRAME"))break;
            unsigned n;
            static ksn_proc_inst code[KSN_PROC_CODE];
            if(strcmp(word,"D")||scanf("%u",&n)!=1||!n||n>KSN_PROC_CODE)return 2;
            for(unsigned i=0;i<n;i++){
                unsigned op,dst,a,b,color;double value;
                if(scanf("%u %u %u %u %lf %u",&op,&dst,&a,&b,&value,&color)!=6)return 2;
                code[i]=(ksn_proc_inst){(uint8_t)op,(uint8_t)dst,(uint8_t)a,(uint8_t)b,(float)value,(uint16_t)color};
            }
            float in[KSN_PROC_INPUTS];
            if(scanf(" %31s",word)!=1||strcmp(word,"I"))return 2;
            for(unsigned j=0;j<KSN_PROC_INPUTS;j++){double v;if(scanf("%lf",&v)!=1)return 2;in[j]=(float)v;}
            const ksn_proc_program p={code,(uint8_t)n};
            if(!ksn_proc_plan_prepare(&plan,&p)){fprintf(stderr,"picture %u: plan rejected\n",pictures);return 1;}
            ksn_proc_status s=ksn_proc_plan_begin(&vm,&plan,in,&frame);
            if(s==KSN_PROC_RUNNING)s=ksn_proc_plan_run(&vm,&plan,false);
            if(s!=KSN_PROC_DONE){
                fprintf(stderr,"picture %u: draw failed (%d) at pc %u after %u steps\n",pictures,(int)s,
                        (unsigned)vm.last_pc,(unsigned)vm.steps);
                return 1;
            }
            for(int y=0;y<KSN_PROC_H;y+=8){
                int h=y+8>KSN_PROC_H?KSN_PROC_H-y:8;
                if(!ksn_proc_render_band(&frame,pixels+y*KSN_PROC_W,y,h))return 1;
            }
            draws++;steps+=vm.steps;entries+=frame.count;raster+=frame.raster_steps;
        }
        if(fwrite(pixels,sizeof pixels,1,out)!=1)return 2;
        printf("%lu %lu %lu %lu\n",draws,steps,entries,raster);
        pictures++;
    }
    fclose(out);
    return 0;
}
