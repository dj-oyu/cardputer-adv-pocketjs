/* Runs Shiki line programs through the C engine (main/ui/kasane/ksn_shiki.c)
 * and prints one line per case, in the text contract the host reference
 * prints (tools/kasane_ir/shiki_ref.mjs), so tools/kasane_ir/shiki_diff.mjs
 * can compare the two byte for byte.
 *
 *   gcc -std=gnu11 -O2 -Imain/ui/kasane main/ui/kasane/ksn_shiki.c \
 *       tools/kasane_contract/shiki_run.c -lm -o shiki_run
 *   ./shiki_run CASES.txt
 *
 * Case:   CASE <hex> ARGS <n> <v...> INPUTS <m> <u...>
 * Output: REJECT
 *         RUN <STATUS> <steps> <penValid> <penX> <penY> R <r0..r15> S <n> <x0 y0 x1 y1 c>...
 * With SHIKI_SIN_TABLE=1 it prints the sine table instead (one line). */
#include "ksn_shiki.h"
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
    static char line[1<<16];
    static uint8_t code[4096];
    static ksn_shiki_plan plan;
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
        const bool ok=ksn_shiki_register(code,len,args,na,&plan);
        const ksn_shiki_status s=ok?ksn_shiki_draw(&plan,in,ni>KSN_SHIKI_INPUTS?KSN_SHIKI_INPUTS:ni,&vm,seg)
                                   :KSN_SHIKI_INVALID;
        ksn_shiki_result(ok,s,&vm,seg,put_stdout,NULL);
        putchar('\n');
    }
    return 0;
}
