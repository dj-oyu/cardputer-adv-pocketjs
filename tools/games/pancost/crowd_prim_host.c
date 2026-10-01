/* Draws crowd_prim.mjs's pictures with the firmware's own VM and band
 * renderer (main/ui/kasane/ksn_procedural.c), and counts what they did.
 * Host only; build with -DKSN_PROC_STATS.
 *
 *   crowd_prim_host OUT.rgb565 < cases.txt
 *
 * cases.txt:
 *   TILES n            then per tile "w h frames key" and w*h*frames texels
 *   FRAME back         a 240 x 135 picture cleared to RGB565 `back`
 *   D n kind           a draw: n rows "op dst a b value color", then
 *                      "I v0 .. v7". kind 0 = scenery, 1 = crowd, 2 = fill
 *   ENDFRAME
 * Each draw runs as pocket_proc.c runs it (a registered plan, then the
 * frame replayed in 8-row bands, top down) over the same pixels, in order.
 * OUT gets every picture, 240 x 135 uint16 little endian. stdout gets one
 * line a picture with the crowd draws' counts (kind 1, and kind 2 apart):
 *   draws steps sins entries extended raster scans hits line_px pattern_px
 *   pattern_written pattern_walk_px tile_rows tile_px tile_written
 * `entries` are frame entries (an extended primitive takes two), `scans`
 * entry x band tests, `hits` the ones that reached a band, pattern_walk_px
 * the pattern pixels drawn by the Bresenham walk (the rest: the horizontal
 * path). */
#define COUNTS 15
#include "ksn_proc_plan.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ksn_proc_frame frame;
static ksn_proc_vm vm;
static ksn_proc_plan plan;
static uint16_t pixels[KSN_PROC_W*KSN_PROC_H];
static ksn_proc_tile tiles[16];
typedef struct { unsigned long v[COUNTS]; } counts;

int main(int argc,char **argv){
    if(argc!=2){fprintf(stderr,"usage: crowd_prim_host OUT < cases\n");return 2;}
    FILE *out=fopen(argv[1],"wb");
    if(!out)return 2;
    char word[32];
    unsigned pictures=0;
    while(scanf("%31s",word)==1){
        if(!strcmp(word,"TILES")){
            unsigned n;
            if(scanf("%u",&n)!=1||n>16)return 2;
            for(unsigned t=0;t<n;t++){
                unsigned w,h,f,key;
                if(scanf("%u %u %u %u",&w,&h,&f,&key)!=4||!w||!h||!f||w>255||h>255||f>255)return 2;
                uint16_t *px=malloc((size_t)w*h*f*sizeof *px);
                if(!px)return 2;
                for(unsigned i=0;i<w*h*f;i++){unsigned v;if(scanf("%u",&v)!=1)return 2;px[i]=(uint16_t)v;}
                tiles[t]=(ksn_proc_tile){px,(uint8_t)w,(uint8_t)h,(uint8_t)f,(uint16_t)key};
            }
            ksn_proc_set_tiles(tiles,n);
            continue;
        }
        if(strcmp(word,"FRAME")){fprintf(stderr,"expected FRAME, got %s\n",word);return 2;}
        unsigned back;
        if(scanf("%u",&back)!=1)return 2;
        for(unsigned i=0;i<KSN_PROC_W*KSN_PROC_H;i++)pixels[i]=(uint16_t)back;
        counts c[3];
        memset(c,0,sizeof c);
        for(;;){
            if(scanf("%31s",word)!=1)return 2;
            if(!strcmp(word,"ENDFRAME"))break;
            unsigned n,kind;
            static ksn_proc_inst code[KSN_PROC_CODE];
            if(strcmp(word,"D")||scanf("%u %u",&n,&kind)!=2||!n||n>KSN_PROC_CODE||kind>2)return 2;
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
            /* The reference stepper first, only to count SINs. */
            unsigned long sins=0;
            ksn_proc_status s=ksn_proc_begin(&vm,&p,in,&frame);
            while(s==KSN_PROC_RUNNING){sins+=code[vm.pc].op==KSN_PROC_SIN;s=ksn_proc_step(&vm);}
            s=ksn_proc_plan_begin(&vm,&plan,in,&frame);
            if(s==KSN_PROC_RUNNING)s=ksn_proc_plan_run(&vm,&plan,false);
            if(s!=KSN_PROC_DONE){
                fprintf(stderr,"picture %u: draw failed (%d) at pc %u after %u steps\n",pictures,(int)s,
                        (unsigned)vm.last_pc,(unsigned)vm.steps);
                return 1;
            }
            memset(&g_ksn_proc_stats,0,sizeof g_ksn_proc_stats);
            for(int y=0;y<KSN_PROC_H;y+=8){
                int h=y+8>KSN_PROC_H?KSN_PROC_H-y:8;
                if(!ksn_proc_render_band(&frame,pixels+y*KSN_PROC_W,y,h))return 1;
            }
            unsigned extended=0,i=0,cost;
            ksn_proc_segment g;
            ksn_proc_seg_kind k;
            while(ksn_proc_frame_next(&frame,&i,&g,&k,&cost))extended+=k!=KSN_PROC_SEG_PLAIN;
            const ksn_proc_stats *t=&g_ksn_proc_stats;
            const unsigned long add[COUNTS]={1,vm.steps,sins,frame.count,extended,frame.raster_steps,t->scans,
                t->entries,t->line_px,t->pattern_px,t->pattern_written,t->pattern_walk_px,
                t->tile_rows,t->tile_px,t->tile_written};
            for(unsigned j=0;j<COUNTS;j++)c[kind].v[j]+=add[j];
        }
        if(fwrite(pixels,sizeof pixels,1,out)!=1)return 2;
        for(unsigned kind=1;kind<3;kind++)for(unsigned j=0;j<COUNTS;j++)
            printf("%lu%c",c[kind].v[j],kind==2&&j==COUNTS-1?'\n':' ');
        pictures++;
    }
    fclose(out);
    return 0;
}
