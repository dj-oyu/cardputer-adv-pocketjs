/* Runs Shiki line programs through the C engine (main/ui/kasane/ksn_shiki.c)
 * and prints one line per case, in the text contract the host reference
 * prints (tools/kasane_ir/shiki_ref.mjs), so tools/kasane_ir/shiki_diff.mjs
 * can compare the two byte for byte.
 *
 *   gcc -std=gnu11 -O2 -Imain/ui/kasane main/ui/kasane/ksn_shiki.c \
 *       tools/kasane_contract/shiki_run.c -lm -o shiki_run
 *   ./shiki_run CASES.txt
 *
 * Case:   CASE <hex> ARGS <n> <v...> INPUTS <m> <u...> [DATA <r> <region>...]
 *         <region> = <type> <q> <k_rec> <n> <e...> | ALIAS <j>  (j: an earlier region)
 * Output: REJECT
 *         RUN <STATUS> <steps> <penValid> <penX> <penY> R <r0..r15> S <n> <x0 y0 x1 y1 c>...
 *             [D <r> (<n> <e...>)...]   (only when the case had DATA)
 * A malformed region (what shiki.data() would refuse: type, q, k_rec, a
 * length that is not whole records or not 1..65,535 of them, an element out
 * of its type's range) makes the case REJECT.
 * With SHIKI_SIN_TABLE=1 it prints the sine table instead (one line). */
#include "ksn_shiki.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *word(char **p){
    while(**p==' ')(*p)++;
    char *s=*p;
    while(**p&&**p!=' '&&**p!='\n')(*p)++;
    if(**p)*(*p)++=0;
    return s;
}
static void put_stdout(void *ctx,const char *t,size_t n){(void)ctx;fwrite(t,1,n,stdout);}
static unsigned hexval(char c){return c<='9'?(unsigned)(c-'0'):(unsigned)((c|32)-'a'+10);}

int main(int argc,char **argv){
    if(getenv("SHIKI_SIN_TABLE")){
        for(unsigned k=0;k<257;k++)printf("%s%ld",k?" ":"",(long)ksn_shiki_sin_table[k]);
        printf("\n");
        return 0;
    }
    FILE *f=argc>1?fopen(argv[1],"r"):stdin;
    if(!f){perror(argv[1]);return 2;}
    static char line[1<<22];
    static uint8_t code[4096];
    static ksn_shiki_vm vm;
    static ksn_proc_segment seg[KSN_SHIKI_SEGMENTS];
    while(fgets(line,sizeof line,f)){
        char *p=line;
        if(strcmp(word(&p),"CASE"))continue;
        const char *hex=word(&p);
        const size_t len=strlen(hex)/2;
        if(len>sizeof code){printf("REJECT\n");continue;}
        for(size_t k=0;k<len;k++)code[k]=(uint8_t)(hexval(hex[2*k])<<4|hexval(hex[2*k+1]));
        double args[64],in[64];
        word(&p);                                   /* ARGS */
        unsigned na=(unsigned)atoi(word(&p));
        for(unsigned k=0;k<na&&k<64;k++)args[k]=strtod(word(&p),NULL);
        word(&p);                                   /* INPUTS */
        unsigned ni=(unsigned)atoi(word(&p));
        for(unsigned k=0;k<ni&&k<64;k++)in[k]=strtod(word(&p),NULL);
        ksn_shiki_region reg[KSN_SHIKI_REGIONS+4];
        void *owned[KSN_SHIKI_REGIONS+4]={0};
        unsigned nr=0;
        bool well=true,data=false;
        if(!strcmp(word(&p),"DATA")){
            data=true;
            nr=(unsigned)atoi(word(&p));
            if(nr>KSN_SHIKI_REGIONS+4){well=false;nr=0;}
            for(unsigned k=0;k<nr;k++){
                const char *w=word(&p);
                if(!strcmp(w,"ALIAS")){
                    const unsigned j=(unsigned)atoi(word(&p));
                    if(j>=k){well=false;reg[k]=(ksn_shiki_region){0};continue;}
                    reg[k]=reg[j];
                    continue;
                }
                const long type=atol(w),q=atol(word(&p)),kr=atol(word(&p));
                const long n=atol(word(&p));
                const size_t es=type==KSN_SHIKI_I32?4:2;
                if(type<0||type>2||q<0||q>30||kr<1||kr>8||n<kr||n%kr||n/kr>KSN_SHIKI_RECORDS)well=false;
                void *mem=well?calloc((size_t)n,es):NULL;
                owned[k]=mem;
                for(long e=0;e<n;e++){
                    const long long v=strtoll(word(&p),NULL,10);
                    const bool in_range=type==0?v>=-32768&&v<=32767:type==1?v>=0&&v<=65535
                                                :v>=INT32_MIN&&v<=INT32_MAX;
                    if(!in_range)well=false;
                    if(!mem)continue;
                    if(type==0)((int16_t *)mem)[e]=(int16_t)v;
                    else if(type==1)((uint16_t *)mem)[e]=(uint16_t)v;
                    else ((int32_t *)mem)[e]=(int32_t)v;
                }
                reg[k]=(ksn_shiki_region){mem,(uint32_t)(n>0?n:0),(uint8_t)type,(uint8_t)q,(uint8_t)kr};
            }
        }
        /* Exactly what the header asks for, so ASan sees an engine that writes
         * past a short plan. */
        const size_t pb=ksn_shiki_plan_bytes_for(code,len);
        ksn_shiki_plan *plan=malloc(pb?pb:sizeof(ksn_shiki_plan));
        const bool ok=well&&pb&&ksn_shiki_register(code,len,args,na,reg,nr,plan);
        const ksn_shiki_status s=ok?ksn_shiki_draw(plan,in,ni>KSN_SHIKI_INPUTS?KSN_SHIKI_INPUTS:ni,&vm,seg)
                                   :KSN_SHIKI_INVALID;
        ksn_shiki_result(ok,s,&vm,seg,put_stdout,NULL);
        if(ok&&data)ksn_shiki_result_regions(reg,nr,put_stdout,NULL);
        putchar('\n');
        for(unsigned k=0;k<KSN_SHIKI_REGIONS+4;k++)free(owned[k]);
        free(plan);
    }
    return 0;
}
