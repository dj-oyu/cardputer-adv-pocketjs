/* ksn_shiki_from_double (spec sec.2.4) against the libm form it replaced
 * (ldexp, clamp, floor), bit for bit, host only. The integer form exists
 * because the S3 runs double arithmetic in software (docs/kasane/shiki-bundle.md).
 *
 *   cc -std=c11 -O2 -Imain/ui/kasane main/ui/kasane/ksn_shiki.c \
 *      tools/kasane_contract/test_shiki_from_double.c -lm && ./a.out
 */
#include "ksn_shiki.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static bool reference(double v,unsigned f,int32_t *out){
    if(!isfinite(v))return false;
    double x=ldexp(v,(int)f);
    if(x<-2147483648.0)x=-2147483648.0;
    if(x>2147483647.0)x=2147483647.0;
    const double r=x<0?-floor(-x+0.5):floor(x+0.5);
    *out=(int32_t)r;
    return true;
}
static unsigned long long st=88172645463325252ull;
static unsigned long long rnd(void){st^=st<<13;st^=st>>7;st^=st<<17;return st;}
static unsigned long checks,bad;
static void one(double v,unsigned f){
    int32_t a=0,b=0;
    const bool ra=reference(v,f,&a),rb=ksn_shiki_from_double(v,f,&b);
    checks++;
    if(ra!=rb||(ra&&a!=b)){if(++bad<=10)printf("DIFF v=%.17g (%a) f=%u ref=%d/%ld got=%d/%ld\n",v,v,f,ra,(long)a,rb,(long)b);}
}
int main(void){
    static const double edge[]={0.0,-0.0,0.5,-0.5,1.5,-1.5,2.5,-2.5,0.49999999999999994,-0.49999999999999994,
        1.0/65536,-1.0/65536,1.0/131072,3.0/131072,2147483647.0,2147483647.4,2147483647.5,2147483646.5,
        -2147483648.0,-2147483648.4,-2147483648.5,-2147483647.5,2147483648.0,-2147483649.0,1e300,-1e300,
        4.9406564584124654e-324,-4.9406564584124654e-324,2.2250738585072014e-308,1e-10,-1e-10,
        32767.5,-32768.5,120.25,-480.0,720.5,0.1,0.2,0.3,1e6,-1e6,INFINITY,-INFINITY,NAN};
    for(unsigned f=0;f<=30;f++){
        for(unsigned k=0;k<sizeof edge/sizeof edge[0];k++)one(edge[k],f);
        /* halves and their neighbours at this scale */
        for(int i=-2000;i<=2000;i++){
            const double h=ldexp(i+0.5,-(int)f);
            one(h,f);one(nextafter(h,INFINITY),f);one(nextafter(h,-INFINITY),f);
            one(ldexp((double)i,-(int)f),f);
        }
    }
    /* random bit patterns (every exponent) and random values in the drawing range */
    for(unsigned long n=0;n<4000000;n++){
        unsigned long long b=rnd();double v;memcpy(&v,&b,sizeof v);
        one(v,(unsigned)(rnd()%31));
        const double w=((double)(int64_t)(rnd()>>11)/(double)(1ull<<53))*2000.0-1000.0;
        one(w,(unsigned)(rnd()%31));
        one(ldexp(w,(int)(rnd()%80)-40),(unsigned)(rnd()%31));
    }
    printf("%s (%lu checks, %lu differ)\n",bad?"SHIKI FROM_DOUBLE FAIL":"SHIKI FROM_DOUBLE PASS",checks,bad);
    return bad?1:0;
}
