/* EMIT_POINTS / EMIT_LINES against a model of their definition (host).
 *
 * ksn_shiki.c's emit_exec was rewritten as one loop without a call and in
 * 32-bit arithmetic (docs/kasane/shiki-jit.md sec.8.3). This compares it, on
 * random programs, with the definition written out plainly: per record,
 * floor((raw + half) / 2^q) by division in double, INVALID for a coordinate
 * outside -480..720 before the record is emitted, LIMIT at 1,024 segments or
 * 8,192 raster steps (a segment costs max(|dx|, |dy|) + 1), the records
 * before the failing one kept. The programs repeat the EMIT so that the
 * limits fall inside a call, and give the start and count through
 * registers, sometimes out of range.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -Imain/ui/kasane main/ui/kasane/ksn_shiki.c \
 *      tools/kasane_contract/test_shiki_emit.c -lm -o shiki-emit && ./shiki-emit
 */
#include "ksn_shiki.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t st=1;
static uint32_t rnd(void){st=st*1664525u+1013904223u;return st>>8;}

typedef struct { uint16_t count,raster; ksn_proc_segment seg[KSN_SHIKI_SEGMENTS]; } model_t;

static int coordm(int32_t raw,unsigned q,int16_t *out){
    const double v=floor(((double)raw+(q?ldexp(1.0,(int)q-1):0.0))/ldexp(1.0,(int)q));
    if(v<-480||v>720)return 0;
    *out=(int16_t)v;
    return 1;
}
/* One EMIT of records start..start+count-1 of planes d (stride S). */
static ksn_shiki_status model_emit(model_t *m,const int16_t *d,uint32_t S,uint32_t start,uint32_t count,
                                   unsigned q,int lines,uint16_t colour){
    for(uint32_t c=start;c<start+count;c++){
        int16_t p[4];
        for(unsigned j=0;j<(lines?4u:2u);j++)if(!coordm(d[j*S+c],q,&p[j]))return KSN_SHIKI_INVALID;
        if(!lines){p[2]=p[0];p[3]=p[1];}
        if(m->count==KSN_SHIKI_SEGMENTS)return KSN_SHIKI_LIMIT;
        const int dx=abs(p[2]-p[0]),dy=abs(p[3]-p[1]);
        const unsigned cost=(unsigned)(dx>dy?dx:dy)+1;
        if(m->raster+cost>KSN_SHIKI_RASTER)return KSN_SHIKI_LIMIT;
        m->raster=(uint16_t)(m->raster+cost);
        m->seg[m->count++]=(ksn_proc_segment){p[0],p[1],p[2],p[3],colour};
    }
    return KSN_SHIKI_RUNNING;
}

static void put(uint8_t *b,uint8_t op,uint8_t dst,uint8_t a,uint8_t bb,int32_t imm,uint16_t aux){
    b[0]=op;b[1]=dst;b[2]=a;b[3]=bb;
    for(unsigned k=0;k<4;k++)b[4+k]=(uint8_t)((uint32_t)imm>>(8*k));
    b[8]=(uint8_t)aux;b[9]=(uint8_t)(aux>>8);b[10]=b[11]=0;
}

int main(void){
    static int16_t data[4*64] __attribute__((aligned(16)));
    static ksn_proc_segment seg[KSN_SHIKI_SEGMENTS];
    static model_t m;
    ksn_shiki_plan *plan=calloc(1,KSN_SHIKI_PLAN_MAX);
    ksn_shiki_vm vm;
    unsigned fails=0,by[5]={0};
    for(unsigned t=0;t<40000;t++){
        const uint32_t R=1+rnd()%60,S=ksn_shiki_stride(R);
        const unsigned q=rnd()%16;
        const int lines=(int)(rnd()&1);
        const unsigned flags=rnd()%4;
        /* Half the programs first emit the records as long lines (raster
         * steps), so that the raster limit also falls inside a points call. */
        const int pre=(int)(rnd()&1);
        /* Mostly in range after the shift, sometimes not; long lines sometimes. */
        const int32_t span=pre?480:rnd()%4==0?32767:(int32_t)((rnd()%3==0?700:300)<<q);
        for(uint32_t k=0;k<4*S;k++){
            int32_t v=(int32_t)(rnd()%(2u*(uint32_t)(span>32767?32767:span)+1u))-(span>32767?32767:span);
            if(v>32767)v=32767;
            data[k]=(int16_t)v;
        }
        int32_t start=(int32_t)(rnd()%(R+1)),count=(int32_t)(rnd()%(R+1));
        if(rnd()%10==0)start=(int32_t)R+1;                      /* out of range */
        if(rnd()%10==0)count=(int32_t)R+5;
        if(start>(int32_t)R)start=(flags&2)?start:0;
        const unsigned reps=1+rnd()%40;
        const uint16_t colour=(uint16_t)rnd();
        uint8_t code[12+4+12*6];
        const uint8_t head[12]={'S','K','I','1',1,0,(uint8_t)(5+pre),0,0,1,0,0};
        memcpy(code,head,12);
        const uint8_t decl[4]={KSN_SHIKI_I16,0,4,0};
        memcpy(code+12,decl,4);
        uint8_t *p=code+16;
        put(p,KSN_SHIKI_SET,0,0,0,start,0);p+=12;
        put(p,KSN_SHIKI_SET,1,0,0,count,0);p+=12;
        put(p,KSN_SHIKI_REPEAT,0,0,0,(int32_t)reps,0);p+=12;
        if(pre){put(p,KSN_SHIKI_EMIT_LINES,0,0,0,0,7);p+=12;}
        /* dst: bit 0 count from a register, bit 1 start; b = 0 (start r0, count r1),
         * or with only the count, b = 1. */
        const uint8_t b=flags==1?1:0;
        put(p,lines?KSN_SHIKI_EMIT_LINES:KSN_SHIKI_EMIT_POINTS,(uint8_t)flags,0,b,(int32_t)q,colour);p+=12;
        put(p,KSN_SHIKI_END,0,0,0,0,0);
        const ksn_shiki_region region={data,R,KSN_SHIKI_I16,0,4};
        if(!ksn_shiki_register(code,12+4+12*(5u+(unsigned)pre),NULL,0,&region,1,plan)){printf("REGISTER FAILED %u\n",t);return 1;}
        const ksn_shiki_status s=ksn_shiki_draw(plan,NULL,0,&vm,seg);
        /* The model: the same repeats, the same start and count rules. */
        memset(&m,0,sizeof m);
        ksn_shiki_status want=KSN_SHIKI_DONE;
        for(unsigned r=0;r<reps&&want==KSN_SHIKI_DONE;r++){
            if(pre){
                const ksn_shiki_status e=model_emit(&m,data,S,0,R,0,1,7);
                if(e!=KSN_SHIKI_RUNNING){want=e;break;}
            }
            uint32_t a=0,n=R;
            if(flags&2){if(start<0||(uint32_t)start>R){want=KSN_SHIKI_INVALID;break;}a=(uint32_t)start;}
            n=R-a;
            if(flags&1){if(count<0||(uint32_t)count>R-a){want=KSN_SHIKI_INVALID;break;}n=(uint32_t)count;}
            const ksn_shiki_status e=model_emit(&m,data,S,a,n,q,lines,colour);
            if(e!=KSN_SHIKI_RUNNING)want=e;
        }
        by[want]++;
        if(s!=want||vm.count!=m.count||vm.raster!=m.raster||memcmp(seg,m.seg,m.count*sizeof *seg)){
            if(fails++<10)printf("DIFF trial %u: status %d/%d count %u/%u raster %u/%u (R=%u q=%u lines=%d flags=%u reps=%u)\n",
                                 t,(int)s,(int)want,vm.count,m.count,vm.raster,m.raster,(unsigned)R,q,lines,flags,reps);
        }
    }
    printf("emit: DONE %u INVALID %u LIMIT %u\n",by[KSN_SHIKI_DONE],by[KSN_SHIKI_INVALID],by[KSN_SHIKI_LIMIT]);
    if(fails){printf("SHIKI EMIT FAIL (%u)\n",fails);return 1;}
    printf("SHIKI EMIT PASS (40000 programs)\n");
    free(plan);
    return 0;
}
