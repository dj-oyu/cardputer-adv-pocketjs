#include "derby_ser_int.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

/* pocket.derby.ser() in integers: docs/apps/derby-ser-division.md sec.4 and
 * derby_ser_int.h. The double ser() in pocket_derby.c is the specification
 * this follows step by step (the same clip, LOD and draw order); the numbers
 * differ by rounding, within the tolerance of sec.5 and the user's decision
 * (a few px at most, invariants exact). Every product of two Q values is
 * 32 x 32 -> 64 bits (mull + mulsh on the S3); 64-bit divisions are left only
 * where the double series divides too (clip points, the LOD's near bound,
 * 1/Z), about 2.4 a chord. */

/* r5sim's per-part cycle counts (-DR5SIM_DERBY_PROF=1; pocket_derby.c owns
 * the arrays and the running mark). */
#ifdef R5SIM_DERBY_PROF
extern uint32_t derby_ser_cy[9],derby_sp_t;
uint32_t pocket_cycles(void);
#define SP(k) do{const uint32_t _t=pocket_cycles();derby_ser_cy[k]+=_t-derby_sp_t;derby_sp_t=_t;}while(0)
#define SC(k) (derby_ser_cy[k]++)
#else
#define SP(k) do{}while(0)
#define SC(k) ((void)0)
#endif
#ifdef DERBY_SER_COUNT
#define CNT(st,f,n) ((st)->f+=(n))
#else
#define CNT(st,f,n) ((void)0)
#endif

typedef int64_t i64;
#define Q16 65536
#define SQRT2_Q30 1518500250        /* sqrt(2) */
#define RSQRT2_Q30 759250125        /* 1/sqrt(2) */

const uint8_t derby_q_prail[8]={16,16,24,24,0,16,0,16};
const uint8_t derby_q_pt[8]={16,16,24,24,0,16,16,24};
const uint8_t derby_q_hl[8]={16,16,16,16,16,16,0,0};
const uint8_t derby_q_crowd[8]={16,16,16,16,16,16,16,16};

/* a x b (Q30 one of them), rounded: 32 x 32 -> 64. */
static inline i64 m30(int32_t a,int32_t b){return ((i64)a*b+(1<<29))>>30;}
/* The same with a 64-bit left factor (the LOD's depth, past 2^31 m Q16 only
 * near its top). */
static inline i64 m30w(i64 a,int32_t b){return (a*b+(1<<29))>>30;}
static inline int32_t fdiv32(int32_t a,int32_t b){int32_t q=a/b;if(a%b&&((a<0)!=(b<0)))q--;return q;}
static inline int32_t cdiv32(int32_t a,int32_t b){return -fdiv32(-a,b);}
static inline i64 fdiv64(i64 a,i64 b){i64 q=a/b;if(a%b&&((a<0)!=(b<0)))q--;return q;}
static inline i64 ab64(i64 v){return v<0?-v:v;}
static inline int32_t sat32(i64 v,derby_ser_stats *st){
    if(v>INT32_MAX){st->sat++;return INT32_MAX;}
    if(v<INT32_MIN){st->sat++;return INT32_MIN;}
    return (int32_t)v;
}
static inline int16_t sat16(int32_t v){return (int16_t)(v>32767?32767:v<-32768?-32768:v);}
/* floor(sqrt(v)), exact: the top 16 bits from v brought under 2^32 by an even
 * shift, then the low bits one by one ((r | b)^2 <= v, a 32 x 32 -> 64
 * square each). Exact matters: the 16-bit root alone, always low by up to
 * 2^-16, moved the LOD's near bound by one level 13 times in the straight's
 * 125,280 app calls (a post 3.8 to 7.2 px off; tools/games/test_derby_serq.c),
 * against 1 with the exact root. */
static DERBY_SER_HOT uint32_t isqrt32(uint32_t v){
    uint32_t r=0,bit=1u<<30;
    while(bit>v)bit>>=2;
    while(bit){
        if(v>=r+bit){v-=r+bit;r=(r>>1)+bit;}else r>>=1;
        bit>>=2;
    }
    return r;
}
static DERBY_SER_HOT i64 isqrt64(i64 v){
    if(v<=0)return 0;
    unsigned sh=0;
    while((uint64_t)v>>sh>0xffffffffull)sh+=2;
    uint32_t r=isqrt32((uint32_t)((uint64_t)v>>sh))<<(sh/2);
    for(uint32_t b=sh?1u<<(sh/2-1u):0u;b;b>>=1){
        const uint32_t t=r|b;
        if((uint64_t)t*t<=(uint64_t)v)r=t;
    }
    return r;
}

/* ---- The PIE boundary (sec.6.3). */
static unsigned lanes(unsigned ends){return (ends+7u)&~7u;}
unsigned derby_ends_soa_bytes(unsigned ends){return 12u*2u*lanes(ends)+15u;}
void derby_ends_soa_init(derby_ends_soa *s,void *mem,unsigned ends){
    const unsigned n=lanes(ends);
    int16_t *p=(int16_t *)(((uintptr_t)mem+15u)&~(uintptr_t)15u);
    s->n=n;
    int16_t **a[12]={&s->rx,&s->rz,&s->tx,&s->tz,&s->v0,&s->v1,&s->v2,&s->v3,&s->v0n,&s->v1n,&s->v2n,&s->v3n};
    for(unsigned i=0;i<12;i++){*a[i]=p;p+=n;}
    memset(s->rx,0,12u*2u*n);
}
DERBY_SER_HOT void derby_ser_rotate_ref(derby_ends_soa *s,int16_t cx,int16_t cz){
    const unsigned n=s->n;
    for(unsigned i=0;i<n;i++){
        const int32_t rx=s->rx[i],rz=s->rz[i],tx=s->tx[i],tz=s->tz[i];
        /* >> on a negative int32 is arithmetic (floor) with GCC. */
        s->v0[i]=sat16((rx*cz-rz*cx)>>14);
        s->v1[i]=sat16((rx*cx+rz*cz)>>14);
        s->v2[i]=sat16((-tz*cz-tx*cx)>>14);
        s->v3[i]=sat16((tx*cz-tz*cx)>>14);
    }
    for(unsigned i=0;i+1<n;i++){s->v0n[i]=s->v0[i+1];s->v1n[i]=s->v1[i+1];s->v2n[i]=s->v2[i+1];s->v3n[i]=s->v3[i+1];}
    s->v0n[n-1]=s->v1n[n-1]=s->v2n[n-1]=s->v3n[n-1]=0;
}
DERBY_SER_HOT uint32_t derby_ser_cull_ref(const derby_ends_soa *s,unsigned chords,const derby_cull_q *p){
    const int32_t w=p->w,f=p->f,nr=p->near,fa=p->far,k=-p->k;
    uint32_t keep=0;
    for(unsigned j=0;j<chords;j++){
        const int32_t d=s->v1[j]+((w*s->v3[j])>>14),l=s->v0[j]+((w*s->v2[j])>>14),
            D=s->v1n[j]+((w*s->v3n[j])>>14),L=s->v0n[j]+((w*s->v2n[j])>>14);
        const bool out=(d<nr&&D<nr)||(d>fa&&D>fa)||(1280*d+l*f<k&&1280*D+L*f<k)||(1280*d-l*f<k&&1280*D-L*f<k);
        if(!out)keep|=1u<<j;
    }
    return keep;
}

/* ---- The frame's inputs. */
DERBY_SER_HOT uint32_t derby_ser_prepare(derby_ends_soa *s,const derby_cam_q *c,const int32_t (*pos)[2],const int16_t (*tan14)[2],
                           unsigned n_ends){
    uint32_t wide=0;
    for(unsigned i=0;i<s->n;i++){
        int32_t rx=0,rz=0,tx=0,tz=0;
        if(i<n_ends){
            /* Q16 -> Q3, rounded half up. */
            const i64 x=((i64)pos[i][0]-c->ex+4096)>>13,z=((i64)pos[i][1]-c->ez+4096)>>13;
            if(ab64(x)>DERBY_Q_REL||ab64(z)>DERBY_Q_REL)wide|=1u<<i;
            else{rx=(int32_t)x;rz=(int32_t)z;tx=tan14[i][0];tz=tan14[i][1];}
        }
        s->rx[i]=(int16_t)rx;s->rz[i]=(int16_t)rz;s->tx[i]=(int16_t)tx;s->tz[i]=(int16_t)tz;
    }
    return wide;
}
DERBY_SER_HOT bool derby_ser_cull_params(const derby_cam_q *c,int32_t w,int32_t m,derby_cull_q *out){
    const i64 w3=((i64)w+4096)>>13;
    if(ab64(w3)>DERBY_Q_W)return false;
    /* margin m + 3 m, Q3 rounded up: covers Q3 positions, the Q14 direction
     * and f in Q3 (sec.6.3; derby_ser_int.h). */
    const i64 mq3=((i64)m+3*Q16+8191)>>13;
    out->w=(int32_t)w3;
    out->f=(int32_t)(((i64)c->f+4096)>>13);
    out->near=(int32_t)((i64)c->f/409600-mq3);                 /* floor(.02 f x 8) */
    i64 far=((i64)c->zf+8191)>>13;far+=mq3;
    out->far=far>INT32_MAX?INT32_MAX:(int32_t)far;
    out->k=(int32_t)(mq3*c->fr3);
    return true;
}
bool derby_ser_cull_wide(const derby_cam_q *c,const int32_t pa[2],const int32_t pb[2],const int32_t ta[2],
                         const int32_t tb[2],int32_t w,int32_t margin_q16){
    const i64 M=(i64)margin_q16+Q16,fr=(i64)c->fr3<<13,k=-((M*fr)>>16);
    bool o[2][4];
    for(int e=0;e<2;e++){
        const int32_t *p=e?pb:pa,*t=e?tb:ta;
        const i64 rx=(i64)p[0]-c->ex,rz=(i64)p[1]-c->ez,
            lat=(rx*c->dz-rz*c->dx)>>30,dep=(rx*c->dx+rz*c->dz)>>30,
            nl=((i64)-t[1]*c->dz-(i64)t[0]*c->dx)>>30,nd=((i64)t[0]*c->dz-(i64)t[1]*c->dx)>>30,
            d=dep+((w*nd)>>30),l=lat+((w*nl)>>30),fl=(c->f*l)>>16;
        o[e][0]=d<(i64)c->zn-M;o[e][1]=d>(i64)c->zf+M;o[e][2]=160*d+fl<k;o[e][3]=160*d-fl<k;
    }
    return (o[0][0]&&o[1][0])||(o[0][1]&&o[1][1])||(o[0][2]&&o[1][2])||(o[0][3]&&o[1][3]);
}

/* ---- The chord tables. */
static bool q16_of(double v,int32_t *out){
    const double x=floor(v*65536.0+.5);
    if(!(x>=-2147483647.0&&x<=2147483647.0))return false;
    *out=(int32_t)x;return true;
}
static bool q30_of(double v,int32_t *out){
    if(!(v>-1.99&&v<1.99))return false;
    *out=(int32_t)floor(v*1073741824.0+.5);return true;
}
bool derby_ser_table(derby_chord_q *rows,const int32_t *ends,unsigned n_ends,int32_t w,int32_t s,int32_t g0,
                     derby_pose_fn pose,void *user){
    for(unsigned j=0;j+1<n_ends;j++){
        derby_chord_q *r=&rows[j];
        /* g0 + Math.round((CH - g0)/s) s, exact in integers (Math.round of
         * a/b: floor((2a + b)/2b), b > 0). */
        const i64 ga=g0+fdiv64(2*((i64)ends[j]-g0)+s,2*(i64)s)*s,gb=g0+fdiv64(2*((i64)ends[j+1]-g0)+s,2*(i64)s)*s;
        memset(r,0,sizeof *r);r->tx=DERBY_CHORD_SKIP;
        if(ga==gb||ab64(ga)>2000000000||ab64(gb)>2000000000)continue;
        double A[4],B[4];
        pose(user,ga/65536.0,w/65536.0,A);pose(user,gb/65536.0,w/65536.0,B);
        const double n=(gb-ga)/65536.0,dx=(B[0]-A[0])/n,dz=(B[1]-A[1])/n,l=sqrt(dx*dx+dz*dz);
        int32_t tx,tz,nx,nz;
        if(!q16_of(A[0],&r->ax)||!q16_of(A[1],&r->az)||!q30_of(dx,&tx)||!q30_of(dz,&tz)||!(l>1e-6)||
           !q30_of(-dz/l,&nx)||!q30_of(dx/l,&nz))return false;
        r->ga=(int32_t)ga;r->gb=(int32_t)gb;r->tx=tx;r->tz=tz;r->nx=nx;r->nz=nz;
    }
    return true;
}

/* ---- One chord. The line in the camera's frame, G (Q16 m) from g0:
 * depth Z(G) = Zq + G uz, lateral X(G) = Xq + G ux (Q16 m; uz, ux Q30).
 * The double series' q1 is Z/f, q0 is X + 120 Z/f (sec.4.2). */
typedef struct {i64 Zq,Xq;int32_t uz,ux;} qline;
static inline i64 Zat(const qline *l,int32_t G){return l->Zq+m30(G,l->uz);}
static inline i64 Xat(const qline *l,int32_t G){return l->Xq+m30(G,l->ux);}
/* The planes of lim(), as values with the double's sign: near Z - f/50, far
 * zf - Z, left f X + 160 Z (x >= -40), right 160 Z - f X (x <= 280). */
static DERBY_SER_HOT i64 plane(const derby_cam_q *c,int p,i64 Z,i64 X){
    const i64 fx=((i64)c->f*X)>>16;
    return p==0?Z-c->zn:p==1?(i64)c->zf-Z:p==2?fx+160*Z:160*Z-fx;
}
/* inr(): in front, and x within -400..640 px. */
static DERBY_SER_HOT bool inrq(const derby_cam_q *c,const qline *l,int32_t G){
    const i64 Z=Zat(l,G),X=Xat(l,G),fx=((i64)c->f*X)>>16;
    return Z>c->zn&&fx+520*Z>0&&520*Z-fx>0;
}
#ifdef DERBY_SER_CHECK
/* The screen point of G on the ground at the series' offset (the join check). */
static bool screen_at(const derby_cam_q *c,const qline *l,int32_t G,double *x,double *y){
    const double Z=Zat(l,G)/65536.0,X=Xat(l,G)/65536.0,f=c->f/65536.0;
    if(!(Z>c->zn/65536.0))return false;
    *x=120+f*X/Z;*y=28+6*f/Z;
    return *x>-400&&*x<640;
}
#endif
DERBY_SER_HOT int derby_ser_chord_q(const derby_cam_q *c,const derby_series_q *p,const derby_chord_q *ch,unsigned j,
                      derby_emit_fn emit,void *user,derby_ser_stats *st){
    (void)j;
    if(ch->tx==DERBY_CHORD_SKIP){SP(2);return 0;}
    st->chords++;SC(7);
    /* The line: A and t rotated (8 products). */
    const i64 rx=(i64)ch->ax-c->ex,rz=(i64)ch->az-c->ez;
    const i64 Za=(rx*c->dx+rz*c->dz+(1<<29))>>30,Xa=(rx*c->dz-rz*c->dx+(1<<29))>>30;
    qline l;
    l.uz=(int32_t)(((i64)ch->tx*c->dx+(i64)ch->tz*c->dz+(1<<29))>>30);
    l.ux=(int32_t)(((i64)ch->tx*c->dz-(i64)ch->tz*c->dx+(1<<29))>>30);
    const int32_t go=ch->ga-p->g0;
    l.Zq=Za-m30(go,l.uz);l.Xq=Xa-m30(go,l.ux);
    CNT(st,mul64,8);
    SP(2);
    /* lim() x 4: both ends' plane values; a plane both are under drops the
     * chord, one under moves that end to the crossing (one division). */
    int32_t lo=go,hi=ch->gb-p->g0;
    for(int k=0;k<4;k++){
        i64 A=plane(c,k,Zat(&l,lo),Xat(&l,lo)),B=plane(c,k,Zat(&l,hi),Xat(&l,hi));
        CNT(st,mul64,6);
        if(A<0&&B<0){SP(3);return 0;}
        if(A>=0&&B>=0)continue;
        /* Normalized under 2^30 (nsau on the device), then lo + (hi - lo)
         * A/(A - B), truncated towards lo..hi's inside. */
        i64 m=ab64(A)>ab64(B)?ab64(A):ab64(B);
        while(m>=(1LL<<30)){A>>=1;B>>=1;m>>=1;}
        if(A==B){SP(3);return 0;}
        const int32_t cut=lo+(int32_t)(((i64)(hi-lo)*A)/(A-B));
        CNT(st,div64,1);
        if(A<0)lo=cut;else hi=cut;
    }
    if(!(lo<hi)){SP(3);return 0;}
    SP(3);
    /* Oriented so that depth rises with G' = o G (the double's o, U, V). */
    const int o=l.uz<0?-1:1;
    const qline f={l.Zq,l.Xq,l.uz*o,l.ux*o};
    const int32_t U=f.uz,s=p->s,t1=o>0?hi:-lo,ne=o>0?lo:-hi;
    /* LOD: Z0 = sqrt(f W s / L), W = |ux Zq - Xq uz| (the double's
     * |v q1 - q0 u| x f^2), held at 1,024 m. */
    i64 W=ab64(((i64)l.ux*l.Zq-l.Xq*(i64)l.uz)>>30);
    if(W>(1LL<<26))W=1LL<<26;
    i64 zsq=(((i64)c->f*W)>>16)*s;
    zsq=p->L==4?zsq>>2:p->L==8?zsq>>3:zsq/p->L;
    i64 Zl=isqrt64(zsq);
    CNT(st,mul64,4);CNT(st,isqrt,1);
    int32_t k=s;
    const i64 Zfar=Zat(&f,t1);
    while(Zl<Zfar&&2*m30(k,U)<((Zfar*19661)>>16)&&k<64*s){k*=2;Zl=m30w(Zl,SQRT2_Q30);CNT(st,mul64,2);}
    int32_t e=cdiv32(t1,k)*k;
    if(!inrq(c,&f,e))e-=k;
    int32_t n0=fdiv32(ne,s)*s;
    if(!inrq(c,&f,n0))n0+=s;
    CNT(st,div32,2);CNT(st,mul64,6);
    /* The draws' last two inputs. */
    int32_t c0=p->a,c1=p->b;
    if(p->turf){
        const int32_t nd=(int32_t)(((i64)ch->nx*c->dx+(i64)ch->nz*c->dz+(1<<29))>>30),
            np=(int32_t)(((i64)ch->nx*c->dz-(i64)ch->nz*c->dx+(1<<29))>>30);
        const i64 dZ=m30(p->a,nd),dX=m30(p->a,np),C1=-((dZ*(i64)(uint32_t)c->invf)>>24);
        c1=sat32(C1,st);c0=sat32(dX-((120*C1)>>8),st);
        CNT(st,mul64,7);
    }
    const bool ut=(i64)U*610>c->f;      /* u > 1e-7 (u = U/f) */
    SP(4);
    int drew=0;
    for(;;){
        Zl=m30w(Zl,RSQRT2_Q30);
        int32_t hh=n0;
        if(k>s){
            /* hh = ceil(max((max(Z0, kU/.3) - q1)/U, n0)/k) k; G past
             * +-2^30 (16,384 m) saturates, beyond any chord. */
            i64 t=m30(k,U)*10/3;
            if(Zl>t)t=Zl;
            int32_t g=-(1<<30);
            if(ut){
                const i64 d=t-f.Zq;
                if(ab64(d)>=U)g=d<0?-(1<<30):(1<<30);
                else{g=(int32_t)((d*(1LL<<30))/U);CNT(st,div64,1);}
            }
            if(n0>g)g=n0;
            hh=cdiv32(g,k)*k;
            CNT(st,div32,1);CNT(st,mul64,2);
        }
        const int32_t cn=(e-hh)/k;
        if(cn>0){
            const i64 Ze=Zat(&f,e),Xe=Xat(&f,e);
            if(Ze<=0)st->bad++;
            else{
                const i64 invf=(uint32_t)c->invf,zq24=(Ze*invf)>>24,ku24=((i64)k*U)>>22,dz24=(ku24*invf)>>32,
                    kx16=m30(k,f.ux);
                int32_t in[8];
                in[0]=sat32(Xe+((120*zq24)>>8),st);
                in[1]=sat32(-(kx16+((120*dz24)>>8)),st);
                in[2]=sat32(-zq24,st);
                in[3]=sat32(dz24,st);
                in[4]=cn>255?255:cn;
                in[5]=sat32(((i64)c->f<<16)/Ze,st);
                in[6]=c0;in[7]=c1;
                CNT(st,mul64,5);CNT(st,div64,1);
#ifdef DERBY_SER_CHECK
                {
                    const i64 r=in[5],z=-(i64)in[2],dz=in[3];
                    if(ab64(r*z-(1LL<<40))>(1LL<<28)+z+r)st->newton_start++;
                    if(k>s&&100*dz>31*(z-(i64)cn*dz))st->newton_step++;
                    if(in[4]<1||in[4]>255)st->count_range++;
                }
#endif
                SP(5);
                st->draws++;drew=1;
                const int rc=emit(user,p->h,in);
                SP(6);
                if(rc)return rc;
            }
        }
        if(k==s)break;
        if(hh<e)e=hh;
        k/=2;
    }
    SP(5);
    if(!p->turf&&p->b<0){
        /* The stands' tiers and crowd: both ends' 1/z and x (4 divisions). */
        const i64 Z0=Zat(&l,lo),X0=Xat(&l,lo),Z1=Zat(&l,hi),X1=Xat(&l,hi);
        if(Z0<=0||Z1<=0)st->bad++;
        else{
            const i64 F=c->f,r0=(F<<16)/Z0,r1=(F<<16)/Z1,xa=(120LL<<16)+F*X0/Z0,xb=(120LL<<16)+F*X1/Z1;
            CNT(st,div64,4);CNT(st,mul64,8);
            const int32_t h[8]={sat32(xa,st),sat32(r0,st),sat32(xb,st),sat32(r1,st),6<<16,-157286,p->hl_lines,21130};
            const int32_t cr[8]={h[0],sat32((28LL<<16)+((r0*314573)>>16),st),h[2],sat32((28LL<<16)+((r1*314573)>>16),st),
                sat32(-((r0*157286)>>16),st),sat32(-((r1*157286)>>16),st),
                sat32(((i64)lo*p->cells)>>16,st),sat32(((i64)hi*p->cells)>>16,st)};
            int rc=0;
            if(p->hl){st->draws++;rc=emit(user,p->hl,h);}
            if(!rc&&p->crowd){st->draws++;rc=emit(user,p->crowd,cr);}
            drew=1;
            SP(6);
            if(rc)return rc;
        }
    }
    if(drew){st->drawn++;SC(8);}
#ifdef DERBY_SER_CHECK
    {
        /* Two chords' shared end (CH[j + 1]): chord j unclipped at gb, chord
         * j + 1 unclipped at ga; their screen points within 1 px. */
        double x,y;
        if(st->last_j>=0&&(unsigned)st->last_j+1u==j&&lo==go&&screen_at(c,&l,lo,&x,&y)){
            const double d=fmax(fabs(x-st->last_x),fabs(y-st->last_y));
            st->joins++;
            if(d>st->join_worst)st->join_worst=d;
            if(d>1)st->join_bad++;
        }
        st->last_j=-1;
        if(hi==ch->gb-p->g0&&screen_at(c,&l,hi,&st->last_x,&st->last_y))st->last_j=(int32_t)j;
    }
#endif
    return 0;
}
