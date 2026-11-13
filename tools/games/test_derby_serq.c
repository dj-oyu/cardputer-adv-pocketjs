/* The integer pocket.derby.ser() (main/pocket/derby_ser_int.c) against the
 * double series on the host: docs/apps/derby-ser-division.md stage S0 (sec.4.3,
 * sec.5). Grown from the design's prototype (tools/games/serq_proto/serq.c):
 * the double side is pocket_derby.c's ser() as it was (VE/VC, ou(), lim(),
 * the LOD and the draws, L >= 1), the integer side is the firmware's own code
 * (tables, the int16 lanes' rotation and cull, the chord), on
 *   1. the app's cameras: three panning units, the leader every 0.37 m from
 *      -20 to 1,010 m, f from 200 to 1,500, the five series of pan() at each
 *      tier, straight and oval;
 *   2. random cameras as __serfuzz draws them (L 4 or 8), the five series
 *      and three random ones.
 * Both sides' draws are turned into the points the plans put on the screen
 * (posts' feet and tops, the turf stripes' ends, the stands' tier ends) and
 * matched both ways (nearest point, max of |dx|, |dy|, on-screen points only).
 *
 * The cull runs on both integer paths, the PIE kernels' (their scalar
 * reference, main/pocket/derby_ser_pie.c with DERBY_SERPIE_HOST: margin
 * m + 1 m) and the scalar one (derby_ser_int.c: m + 3 m); the integer side
 * draws with the PIE path's, the product's default (sec.13).
 *
 * Gate (the user's decision, sec. "利用者の決定"): no saturated input, no
 * skipped draw, Newton's start and step, the line counts, the chords' joins
 * within 1 px, both integer culls keeping every chord the double ou() keeps,
 * no call where the double draws on the screen and the integers draw
 * nothing, and calls with a point over 2 px apart (a tie flipped: one post
 * or stripe) at most 1 in 10,000. The distances are recorded, not gated.
 *
 *   gcc -O2 -DDERBY_SER_CHECK -DDERBY_SER_COUNT -DDERBY_SERPIE_HOST -Imain/pocket tools/games/test_derby_serq.c \
 *       main/pocket/derby_ser_int.c main/pocket/derby_ser_pie.c -lm -o test-derby-serq && ./test-derby-serq [fuzz cases]
 * (tools/games/run_derby.py builds and runs it with 10,000 fuzz cases; the
 * full 40,000 of the design's table by hand.) */
#include "derby_ser_int.h"
#include "derby_ser_pie.h"
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
typedef int64_t i64;
#define QM 65536.0

/* ---- The course (DERBY's straight and oval, apps/derby/derby_view.js). */
static int nsec,nends;
static double sec[4][7],dnr=11,ch[32];
static int32_t qch[32],qpos[32][2],qt30[32][2];
static int16_t qt14[32][2];
static dsp_ends dsE;
static dsp_rot_k dsK;
static void pose(double g,double w,double o[4]){
    if(!nsec){o[0]=g;o[1]=w;o[2]=1;o[3]=0;return;}
    int i=0;
    while(i<nsec-1&&g>sec[i][0])g-=sec[i++][0];
    const double *e=sec[i];
    double x=e[2],z=e[3],c=e[4],d=e[5];
    if(e[1]!=0){const double r=1/e[1]+dnr,C=cos(e[6]+e[1]*g),S=sin(e[6]+e[1]*g);x+=(S-d)*r;z-=(C-c)*r;c=C;d=S;g=0;}
    o[0]=x+g*c-w*d;o[1]=z+g*d+w*c;o[2]=c;o[3]=d;
}
static void pose_cb(void *u,double g,double w,double o[4]){(void)u;pose(g,w,o);}
static int nslot;
static int32_t key[DERBY_Q_SLOTS][3];
static derby_chord_q rows[DERBY_Q_SLOTS][32];
static void course(bool oval){
    const double OB=17891910;
    nends=0;ch[nends++]=-655360000/QM;
    if(oval)for(int j=0;j<=16;j++)ch[nends++]=(OB+j*1544156.0)/QM;
    ch[nends++]=655360000/QM;
    nsec=0;
    if(oval){
        const double OC[3][7]={{OB,0,60490310,-14286848,-65536,0,0},{24706490,-7864320,42598400,-14286848,-65536,0,32768},
                               {131072000,0,42598400,0,65536,0,0}};
        nsec=3;
        for(int i=0;i<3;i++){for(int k=0;k<7;k++)sec[i][k]=OC[i][k]/QM;if(sec[i][1]!=0){sec[i][1]=1/sec[i][1];sec[i][6]*=2*M_PI;}}
    }
    for(int i=0;i<nends;i++){
        double m[4];pose(ch[i],0,m);qch[i]=(int32_t)(ch[i]*65536);
        qpos[i][0]=(int32_t)floor(m[0]*65536+.5);qpos[i][1]=(int32_t)floor(m[1]*65536+.5);
        qt30[i][0]=(int32_t)floor(m[2]*1073741824.0+.5);qt30[i][1]=(int32_t)floor(m[3]*1073741824.0+.5);
        qt14[i][0]=(int16_t)floor(m[2]*16384+.5);qt14[i][1]=(int16_t)floor(m[3]*16384+.5);
    }
    int32_t nx[32],nz[32];
    for(int i=0;i<nends;i++){nx[i]=-qt30[i][1];nz[i]=qt30[i][0];}
    dsp_set_course(&dsE,(unsigned)nends,nx,nz);
    nslot=0;
}

/* ---- The camera: aim() and view() of pocket_derby.c, doubles, and camq. */
static double pc[6],fr,zf,VE[32*4],cells=2.5;
static int vc[32],in_view,hl_lines=4;
static derby_cam_q cq;
static derby_ends_soa soa;
static _Alignas(16) uint8_t soa_mem[1024];
static uint32_t qvc,qwide;
static void aim(double gu,double wu,double gl,double fmin,double px,double fmax){
    double q[4],a[4];pose(gu,wu,q);pose(gl,(dnr+22.6)/2,a);
    const double x=a[0]-q[0],z=a[1]-q[1],e=sqrt(x*x+z*z),f=px*e/2.4;
    pc[0]=q[0];pc[1]=q[1];pc[2]=x/e;pc[3]=z/e;pc[4]=f<fmin?fmin:f>fmax?fmax:f;pc[5]=e;fr=sqrt(pc[4]*pc[4]+25600);
}
static bool ou(int j,double w,double m,double zf_){
    const double *V=VE+4*j,f=pc[4],k=m*fr,l=V[0]+w*V[2],d=V[1]+w*V[3],L=V[4]+w*V[6],D=V[5]+w*V[7];
    return (d<.02*f-m&&D<.02*f-m)||(d>zf_+m&&D>zf_+m)||(160*d+l*f<-k&&160*D+L*f<-k)||(160*d-l*f<-k&&160*D-L*f<-k);
}
static long cull_calls,cull_lost,cull_extra,cull_kept,pie_lost,pie_extra,pie_scalar_diff;
/* pocket_derby.c's cull() on both paths. */
static uint32_t cull_path(bool pie,int32_t w,int32_t m){
    const unsigned chords=(unsigned)nends-1u;
    const uint32_t all=(1u<<chords)-1u;
    uint32_t keep,wc;
    if(pie){
        dsp_cull_k k;
        if(!dsp_cull_consts(&k,cq.f,cq.fr16,cq.zf,w,m))return all;
        keep=dsE.valid&~dsp_cull_ref(&dsE,&k);
        wc=all&~dsE.valid;
    }else{
        derby_cull_q p;
        if(!derby_ser_cull_params(&cq,w,m,&p))return all;
        wc=(qwide|qwide>>1)&all;
        keep=derby_ser_cull_ref(&soa,chords,&p)&~wc;
    }
    for(uint32_t b=wc;b;b&=b-1u){
        const unsigned j=(unsigned)__builtin_ctz(b);
        if(!derby_ser_cull_wide(&cq,qpos[j],qpos[j+1],qt30[j],qt30[j+1],w,m))keep|=1u<<j;
    }
    return keep;
}
static uint32_t keep_mask(int32_t w,int32_t m){
    const uint32_t keep=cull_path(true,w,m),ks=cull_path(false,w,m);
    /* Conservative: every chord the double ou() keeps, on both paths. */
    if(nsec){
        cull_calls++;
        for(unsigned j=0;j<(unsigned)nends-1u;j++){
            const bool d=!ou((int)j,w/QM,m/QM,zf),q=ks>>j&1,p=keep>>j&1;
            if(d&&!q)cull_lost++;
            if(q&&!d)cull_extra++;
            if(d&&!p)pie_lost++;
            if(p&&!d)pie_extra++;
            if(d)cull_kept++;
            if(p!=q)pie_scalar_diff++;
        }
    }
    return keep;
}
static void view(double extra){
    zf=pc[5]+extra;in_view=0;
    if(nsec){
        for(int i=0;i<nends;i++){
            double m[4];pose(ch[i],0,m);
            const double x=m[0]-pc[0],z=m[1]-pc[1];
            double *V=VE+4*i;
            V[0]=x*pc[3]-z*pc[2];V[1]=x*pc[2]+z*pc[3];V[2]=-m[3]*pc[3]-m[2]*pc[2];V[3]=m[2]*pc[3]-m[3]*pc[2];
        }
        for(int j=1;j<nends;j++)if(!ou(j-1,25.5,30,zf))vc[in_view++]=j-1;
    }else vc[in_view++]=0;
    /* camq as pocket_derby.c's cam_q(). */
    cq.ex=(int32_t)floor(pc[0]*65536+.5);cq.ez=(int32_t)floor(pc[1]*65536+.5);
    cq.dx=(int32_t)floor(pc[2]*1073741824.0+.5);cq.dz=(int32_t)floor(pc[3]*1073741824.0+.5);
    cq.cx=(int16_t)floor(pc[2]*16384+.5);cq.cz=(int16_t)floor(pc[3]*16384+.5);
    cq.f=(int32_t)floor(pc[4]*65536+.5);cq.invf=(int32_t)floor(4294967296.0/pc[4]+.5);
    cq.zf=(int32_t)floor(zf*65536+.5);cq.zn=cq.f/50;cq.fr3=(int32_t)ceil(fr*8);cq.fr16=(int32_t)ceil(fr*65536);
    if(nsec){
        derby_ends_soa_init(&soa,soa_mem,(unsigned)nends);
        qwide=derby_ser_prepare(&soa,&cq,(const int32_t (*)[2])qpos,(const int16_t (*)[2])qt14,(unsigned)nends);
        derby_ser_rotate_ref(&soa,cq.cx,cq.cz);
        dsp_set_camera(&dsE,&dsK,&qpos[0][0],&qpos[0][1],2,cq.ex,cq.ez,cq.dx,cq.dz);
        dsp_rotate_ref(&dsE,&dsK);
        qvc=keep_mask(1671168,30*65536);
    }else qvc=1;
}

/* ---- Draws: both sides as doubles in the plans' inputs. */
enum{K_PRAIL,K_PT,K_HL,K_CROWD};
typedef struct{int kind;double in[8];}draw_t;
#define MAXD 512
static draw_t RD[MAXD],ID[MAXD];
static int rn,in_;
static void rdraw(int kind,const double *in){if(rn<MAXD){RD[rn].kind=kind;memcpy(RD[rn].in,in,sizeof RD[rn].in);rn++;}}
static const uint8_t *const QF[4]={derby_q_prail,derby_q_pt,derby_q_hl,derby_q_crowd};
static int emit(void *u,uint32_t h,const int32_t in[8]){
    (void)u;
    const int kind=h==2?K_HL:h==3?K_CROWD:h==4?K_PT:K_PRAIL;
    if(in_<MAXD){ID[in_].kind=kind;for(int i=0;i<8;i++)ID[in_].in[i]=in[i]/ldexp(1,QF[kind][i]);in_++;}
    return 0;
}
/* pocket_derby.c's double ser() for one call (L != 0). */
static double jr(double a){return floor(a+.5);}
static bool inr(const double q[2],double U,double V,double e){const double z=q[1]+e*U,x=(q[0]+e*V)/z;return z>.02&&x>-400&&x<640;}
static void lim(double *lo,double *hi,double c0,double c1){
    if(c1>0){const double v=-c0/c1;if(v>*lo)*lo=v;}else if(c1<0){const double v=-c0/c1;if(v<*hi)*hi=v;}else if(c0<0)*hi=-1e9;
}
static void ser_ref(bool turf,double w,double s,double g0,double L,double a,double b){
    const double zp=zf/pc[4];
    for(int i=0;i<in_view;i++){
        const int j=vc[i];
        if(nsec&&ou(j,w,s,zf))continue;
        const double ga=g0+jr((ch[j]-g0)/s)*s,gb=g0+jr((ch[j+1]-g0)/s)*s;
        double A[4],B[4];pose(ga,w,A);pose(gb,w,B);
        const double dx=(B[0]-A[0])/(gb-ga),dz=(B[1]-A[1])/(gb-ga),X=A[0]+(g0-ga)*dx-pc[0],Y=A[1]+(g0-ga)*dz-pc[1],
            Q=(X*pc[2]+Y*pc[3])/pc[4],q[2]={X*pc[3]-Y*pc[2]+120*Q,Q},u=(dx*pc[2]+dz*pc[3])/pc[4],v=dx*pc[3]-dz*pc[2]+120*u;
        double Lo=ga-g0,Hi=gb-g0;
        lim(&Lo,&Hi,q[1]-.02,u);lim(&Lo,&Hi,zp-q[1],-u);lim(&Lo,&Hi,q[0]+40*q[1],v+40*u);lim(&Lo,&Hi,280*q[1]-q[0],280*u-v);
        if(!(Lo<Hi))continue;
        double c0=a,c1=b;
        if(turf){const double l=a/sqrt(dx*dx+dz*dz),x=-dz*l,z=dx*l;c1=-(x*pc[2]+z*pc[3])/pc[4];c0=x*pc[3]-z*pc[2]-120*c1;}
        const double o=u<0?-1:1,U=u*o,V=v*o,t1=o>0?Hi:-Lo;
        double k=s,Z=sqrt(fabs(v*q[1]-q[0]*u)*s/L),n0=floor((o>0?Lo:-Hi)/s)*s,e;
        while(Z<q[1]+t1*U&&2*k*U<.3*(q[1]+t1*U)&&k<64*s)k*=2,Z*=M_SQRT2;
        e=ceil(t1/k)*k;
        if(!inr(q,U,V,e))e-=k;
        if(!inr(q,U,V,n0))n0+=s;
        for(;;){
            Z/=M_SQRT2;
            const double t=Z>k*U/.3?Z:k*U/.3;
            double g=U>1e-7?(t-q[1])/U:-1e9;
            if(n0>g)g=n0;
            const double hh=k>s?ceil(g/k)*k:n0,c=jr((e-hh)/k),z=q[1]+e*U;
            if(c>0){const double in[8]={q[0]+e*V,-V*k,-z,U*k,c>255?255:c,1/z,c0,c1};rdraw(turf?K_PT:K_PRAIL,in);}
            if(k==s)break;
            if(hh<e)e=hh;
            k/=2;
        }
        if(!turf&&b<0){
            const double y=q[1]+Lo*u,z=q[1]+Hi*u,xa=(q[0]+Lo*v)/y,xb=(q[0]+Hi*v)/z;
            const double h[8]={xa,1/y,xb,1/z,6,-2.4,hl_lines,21130},c[8]={xa,28+4.8/y,xb,28+4.8/z,-2.4/y,-2.4/z,Lo*cells,Hi*cells};
            rdraw(K_HL,h);rdraw(K_CROWD,c);
        }
    }
}
/* The integer call, as pocket_derby.c's ser_q() (tables by (w, s, g0)). */
static derby_ser_stats st;
static void ser_int(bool turf,int32_t w,int32_t s,int32_t g0,int32_t L,int32_t a,int32_t b){
    int k=0;
    while(k<nslot&&!(key[k][0]==w&&key[k][1]==s&&key[k][2]==g0))k++;
    if(k==nslot){
        if(nslot==(int)DERBY_Q_SLOTS){fprintf(stderr,"slots\n");exit(2);}
        nslot++;
        if(!derby_ser_table(rows[k],qch,(unsigned)nends,w,s,g0,pose_cb,NULL)){fprintf(stderr,"table out of range\n");exit(2);}
        key[k][0]=w;key[k][1]=s;key[k][2]=g0;
    }
    const uint32_t keep=nsec?qvc&keep_mask(w,s):qvc;
    const derby_series_q p={.s=s,.g0=g0,.L=L,.a=a,.b=turf?0:b,.turf=turf,.hl_lines=hl_lines,.cells=(int32_t)(cells*65536),
        .h=turf?4:1,.hl=2,.crowd=3};
#ifdef DERBY_SER_CHECK
    st.last_j=-1;
#endif
    for(uint32_t m=keep;m;m&=m-1u)derby_ser_chord_q(&cq,&p,&rows[k][__builtin_ctz(m)],(unsigned)__builtin_ctz(m),emit,NULL,&st);
}

/* ---- The comparison. */
typedef struct{double x,y;}pt_t;
static int points(const draw_t *D,int n,pt_t *P,int max){
    int m=0;
    for(int i=0;i<n;i++){
        const double *in=D[i].in;
        if(D[i].kind==K_PRAIL||D[i].kind==K_PT){
            const int cnt=(int)in[4];
            for(int j=0;j<=cnt&&m+2<max;j++){
                if(D[i].kind==K_PT&&j==cnt)break;
                const double px=in[0]+j*in[1],pz=in[2]+j*in[3],q=-1/pz;
                P[m++]=(pt_t){px*q,28+6*q};
                if(D[i].kind==K_PRAIL)P[m++]=(pt_t){px*q,28+q*in[7]};
                else{const double qf=-1/(pz+in[7]);P[m++]=(pt_t){(px+in[6])*qf,28+6*qf};}
            }
        }else if(D[i].kind==K_HL&&m+4<max){
            P[m++]=(pt_t){in[0],28+6*in[1]};P[m++]=(pt_t){in[2],28+6*in[3]};
            P[m++]=(pt_t){in[0],28+(6-2.4*in[6])*in[1]};P[m++]=(pt_t){in[2],28+(6-2.4*in[6])*in[3]};
        }
    }
    return m;
}
static long hist[6],npts,calls,draw_diff,cnt_ref,cnt_int,swap_calls,missing,on_a,on_b;
static double worst;
static bool on(pt_t p){return p.x>=0&&p.x<=240&&p.y>=-20&&p.y<=155;}
static double match(const pt_t *A,int na,const pt_t *B,int nb,long *n_on){
    double w=0;
    for(int i=0;i<na;i++){
        if(!on(A[i]))continue;
        ++*n_on;
        double best=1e9;
        for(int j=0;j<nb;j++){const double d=fmax(fabs(A[i].x-B[j].x),fabs(A[i].y-B[j].y));if(d<best)best=d;}
        npts++;
        hist[best<=.01?0:best<=.1?1:best<=.5?2:best<=1?3:best<=2?4:5]++;
        if(best>w)w=best;
    }
    return w;
}
static pt_t PA[8192],PB[8192];
static int verbose;
static void compare(const char *tag){
    calls++;cnt_ref+=rn;cnt_int+=in_;if(rn!=in_)draw_diff++;
    const int na=points(RD,rn,PA,8192),nb=points(ID,in_,PB,8192);
    long oa=0,ob=0;
    const double w=fmax(match(PA,na,PB,nb,&oa),match(PB,nb,PA,na,&ob));
    on_a+=oa;on_b+=ob;
    if(oa&&!ob)missing++;
    if(w>2)swap_calls++;
    if(w>worst)worst=w;
    if(verbose&&w>2)printf("  %.2f px at %s call %ld: double %d draws, int %d draws, f %.1f\n",w,tag,calls,rn,in_,pc[4]);
}
static void one(const char *tag,bool turf,double w,double s,double g0,int L,double a,double b){
    rn=in_=0;
    ser_ref(turf,w,s,g0,L,a,b);
    ser_int(turf,(int32_t)(w*65536),(int32_t)(s*65536),(int32_t)(g0*65536),L,turf?(int32_t)(a*65536):(int32_t)a,
            (int32_t)(b*65536));
    compare(tag);
}
static uint32_t rs=12345;
static double rnd(void){rs^=rs<<13;rs^=rs>>17;rs^=rs<<5;return rs/4294967296.0;}
static const double KN[3][2]={{8,10},{5,7},{4,5}};
static void frame(int tier,int L){
    hl_lines=(int)KN[tier][0];
    one("stands",false,40,12,0,L,31727,-7.5);
    one("outer",false,1481114/QM,KN[tier][0],0,L,0xad55,321126/QM);
    one("turf0",true,11,2*KN[tier][1],0,L,760218/QM,0);
    one("turf1",true,11,2*KN[tier][1],KN[tier][1],L,760218/QM,0);
    one("inner",false,11,KN[tier][0],0,L,0xffff,321126/QM);
}
static int failures;
static void report(const char *name,double swap_limit){
    printf("%s: calls %ld, draws double %ld int %ld (calls with another draw count %ld)\n",name,calls,cnt_ref,cnt_int,draw_diff);
    printf("  on-screen points %ld: <=0.01 px %ld, <=0.1 %ld, <=0.5 %ld, <=1 %ld, <=2 %ld, >2 %ld; worst %.4f px\n",
           npts,hist[0],hist[1],hist[2],hist[3],hist[4],hist[5],worst);
    printf("  calls with a point over 2 px %ld (limit %.0f), calls drawn by the double only %ld\n",swap_calls,swap_limit,missing);
    printf("  cull: %ld series x chords kept by ou() %ld; PIE path (drawn) lost %ld, extra %ld (%.1f%%); scalar lost %ld, "
           "extra %ld (%.1f%%); kept by one path only %ld\n",cull_calls,cull_kept,pie_lost,pie_extra,
           cull_kept?100.0*pie_extra/cull_kept:0,cull_lost,cull_extra,cull_kept?100.0*cull_extra/cull_kept:0,pie_scalar_diff);
    printf("  integers: chords %u (drawn %u), draws %u, saturated %u, skipped %u",st.chords,st.drawn,st.draws,st.sat,st.bad);
#ifdef DERBY_SER_CHECK
    printf(", newton start %u step %u, counts %u, joins %u (over 1 px %u, worst %.4f px)",st.newton_start,st.newton_step,
           st.count_range,st.joins,st.join_bad,st.join_worst);
#endif
    printf("\n");
#ifdef DERBY_SER_COUNT
    printf("  per chord: mul64 %.1f, div64 %.2f, div32 %.2f, isqrt %.2f\n",(double)st.mul64/st.chords,(double)st.div64/st.chords,
           (double)st.div32/st.chords,(double)st.isqrt/st.chords);
#endif
    bool bad=st.sat||st.bad||cull_lost||pie_lost||missing||swap_calls>swap_limit;
#ifdef DERBY_SER_CHECK
    bad=bad||st.newton_start||st.newton_step||st.count_range||st.join_bad;
#endif
    if(bad){printf("  FAIL\n");failures++;}
    memset(hist,0,sizeof hist);npts=calls=draw_diff=cnt_ref=cnt_int=swap_calls=missing=on_a=on_b=0;worst=0;
    cull_calls=cull_lost=cull_extra=cull_kept=pie_lost=pie_extra=pie_scalar_diff=0;memset(&st,0,sizeof st);
}
int main(int argc,char **argv){
    const int cases=argc>1?atoi(argv[1]):40000;
    verbose=argc>2;
    /* 1. The app's cameras. */
    for(int oval=0;oval<2;oval++){
        course(oval);
        for(int tier=0;tier<3;tier++){
            nslot=0;
            for(int u=0;u<3;u++)for(double g=-20;g<=1010;g+=0.37){
                const double ug[3]={100,460,820},uw=oval&&u==1?-3:-14;
                aim(ug[u],uw,g,200,14,1500);view(tier?75:150);
                frame(tier,((int)(g*7))&1?8:4);
            }
        }
        report(oval?"app cameras, oval":"app cameras, straight",calls/10000.0);
    }
    /* 2. Random cameras (as __serfuzz, L != 0). */
    for(int n=0;n<cases;n++){
        const bool oval=rnd()<.7;
        course(oval);
        const int tier=(int)(rnd()*3);
        const double g=-150+1300*rnd();
        double gc,wc,f;
        switch((int)(rnd()*5)){case 0:gc=g;break;case 1:gc=g-40+80*rnd();break;case 2:gc=100+900*rnd();break;case 3:gc=460;break;default:gc=820;}
        switch((int)(rnd()*5)){case 0:wc=-14;break;case 1:wc=-100;break;case 2:wc=-3;break;case 3:wc=-150+210*rnd();break;default:wc=11+30*rnd();}
        switch((int)(rnd()*4)){case 0:f=200;break;case 1:f=1500;break;case 2:f=0;break;default:f=50+(int)(3000*rnd());}
        if(f)aim(gc,wc,g,f,14,f);else aim(gc,wc,g,200,14,1500);
        const double ex[3]={75,150,(int)(500*rnd())};
        view(ex[(int)(rnd()*3)]);
        frame(tier,rnd()<.5?4:8);
        for(int i=0;i<3;i++){
            const double ss[10]={1,2.5,4,5,7,8,10,12,14,16*rnd()+.5},s=ss[(int)(rnd()*10)],g0s[3]={0,s/2,s*rnd()},
                g0=g0s[(int)(rnd()*3)];
            const double w=5+45*rnd();
            const int bi=(int)(rnd()*5);
            const double sq=floor(s*65536)/65536,g0q=floor(g0*65536)/65536,wq=floor(w*65536)/65536;
            if(bi==4)one("fz-turf",true,wq,sq,g0q,rnd()<.5?4:8,floor((5+20*rnd())*65536)/65536,0);
            else{const double bs[4]={-7.5,4.9,-1-9*rnd(),9*rnd()};one("fz-rail",false,wq,sq,g0q,rnd()<.5?4:8,31727,floor(bs[bi]*65536)/65536);}
        }
    }
    report("random cameras (fuzz)",calls/10000.0);
    printf("%s\n",failures?"DERBY_SERQ FAIL":"DERBY_SERQ PASS");
    return failures?1:0;
}
