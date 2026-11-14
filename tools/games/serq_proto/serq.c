// Prototype: pocket.derby.ser() in fixed point against the double original.
// Host only; a design experiment for docs/apps/derby-ser-division.md.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
typedef int64_t i64;
#define QM 65536.0
// ---- course (DERBY's oval, apps/derby/derby_view.js)
static int nsec; static double sec[4][7], dnr=11; static double ch[32]; static int nends;
static void course(bool oval){
  const double OB=17891910;
  nends=0; ch[nends++]=-655360000/QM;
  if(oval){for(int j=0;j<=16;j++)ch[nends++]=(OB+j*1544156.0)/QM;}
  ch[nends++]=655360000/QM;
  nsec=0;
  if(oval){
    const double OC[3][7]={{OB,0,60490310,-14286848,-65536,0,0},{24706490,-7864320,42598400,-14286848,-65536,0,32768},{131072000,0,42598400,0,65536,0,0}};
    nsec=3;
    for(int i=0;i<3;i++){for(int k=0;k<7;k++)sec[i][k]=OC[i][k]/QM; if(sec[i][1]!=0){sec[i][1]=1/sec[i][1];sec[i][6]*=2*M_PI;}}
  }
}
static void pose(double g,double w,double o[4]){
  if(!nsec){o[0]=g;o[1]=w;o[2]=1;o[3]=0;return;}
  int i=0; while(i<nsec-1&&g>sec[i][0])g-=sec[i++][0];
  const double*e=sec[i]; double x=e[2],z=e[3],c=e[4],d=e[5];
  if(e[1]!=0){const double r=1/e[1]+dnr,C=cos(e[6]+e[1]*g),S=sin(e[6]+e[1]*g);x+=(S-d)*r;z-=(C-c)*r;c=C;d=S;g=0;}
  o[0]=x+g*c-w*d;o[1]=z+g*d+w*c;o[2]=c;o[3]=d;
}
// ---- camera, view
static double pc[6],fr,zf,VE[32*4],hl_lines=4,cells=2.5; static int vc[32],in_view;
static void aim(double gu,double wu,double gl,double fmin,double px,double fmax){
  double q[4],a[4];pose(gu,wu,q);pose(gl,(dnr+22.6)/2,a);
  double x=a[0]-q[0],z=a[1]-q[1],e=sqrt(x*x+z*z);
  pc[0]=q[0];pc[1]=q[1];pc[2]=x/e;pc[3]=z/e;pc[4]=fmin>px*e/2.4?fmin:(px*e/2.4>fmax?fmax:px*e/2.4);pc[5]=e;fr=sqrt(pc[4]*pc[4]+25600);
}
static bool ou(int j,double w,double m,double zf_){
  const double*V=VE+4*j,f=pc[4],k=m*fr,l=V[0]+w*V[2],d=V[1]+w*V[3],L=V[4]+w*V[6],D=V[5]+w*V[7];
  return (d<.02*f-m&&D<.02*f-m)||(d>zf_+m&&D>zf_+m)||(160*d+l*f<-k&&160*D+L*f<-k)||(160*d-l*f<-k&&160*D-L*f<-k);
}
static void view(double extra){
  zf=pc[5]+extra; in_view=0;
  if(nsec){
    for(int i=0;i<nends;i++){double m[4];pose(ch[i],0,m);double x=m[0]-pc[0],z=m[1]-pc[1],*V=VE+4*i;
      V[0]=x*pc[3]-z*pc[2];V[1]=x*pc[2]+z*pc[3];V[2]=-m[3]*pc[3]-m[2]*pc[2];V[3]=m[2]*pc[3]-m[3]*pc[2];}
    for(int j=1;j<nends;j++)if(!ou(j-1,25.5,30,zf))vc[in_view++]=j-1;
  }else vc[in_view++]=0;
}
// ---- draws
enum{K_PRAIL,K_PT,K_HL,K_CROWD};
static const int QF[4][8]={{16,16,24,24,0,16,0,16},{16,16,24,24,0,16,16,24},{16,16,16,16,16,16,0,0},{16,16,16,16,16,16,16,16}};
typedef struct{int kind;double in[8];}draw_t;
#define MAXD 512
static draw_t RD[MAXD],ID[MAXD]; static int rn,in_;
static double quant(double v,int q){double x=v*ldexp(1,q);if(x>2147483647.)x=2147483647.;if(x<-2147483648.)x=-2147483648.;x=x<0?-floor(-x+.5):floor(x+.5);return x/ldexp(1,q);}
static void rdraw(int kind,const double*in){if(rn<MAXD){RD[rn].kind=kind;for(int i=0;i<8;i++)RD[rn].in[i]=quant(in[i],QF[kind][i]);rn++;}}
static i64 sat32(i64 v){return v>2147483647?2147483647:v<-2147483648LL?-2147483648LL:v;}
static long sat_events;
static void idraw(int kind,const i64*in){if(in_<MAXD){ID[in_].kind=kind;for(int i=0;i<8;i++){if(in[i]!=sat32(in[i]))sat_events++;ID[in_].in[i]=(double)sat32(in[i])/ldexp(1,QF[kind][i]);}in_++;}}
// ---- reference: main/pocket/pocket_derby.c js_ser (L != 0 only)
static double jr(double a){return floor(a+.5);} // Math.round for the values here
static bool inr(const double q[2],double U,double V,double e){const double z=q[1]+e*U,x=(q[0]+e*V)/z;return z>.02&&x>-400&&x<640;}
static void lim(double*lo,double*hi,double c0,double c1){if(c1>0){double v=-c0/c1;if(v>*lo)*lo=v;}else if(c1<0){double v=-c0/c1;if(v<*hi)*hi=v;}else if(c0<0)*hi=-1e9;}
static long ref_chords,ref_drawn_chords;
static void ser_ref(bool turf,double w,double s,double g0,double L,double a,double b){
  const double zp=zf/pc[4];
  for(int i=0;i<in_view;i++){
    const int j=vc[i];
    if(nsec&&ou(j,w,s,zf))continue;
    ref_chords++;
    const double ga=g0+jr((ch[j]-g0)/s)*s,gb=g0+jr((ch[j+1]-g0)/s)*s;
    double A[4],B[4];pose(ga,w,A);pose(gb,w,B);
    const double dx=(B[0]-A[0])/(gb-ga),dz=(B[1]-A[1])/(gb-ga),X=A[0]+(g0-ga)*dx-pc[0],Y=A[1]+(g0-ga)*dz-pc[1],
      Q=(X*pc[2]+Y*pc[3])/pc[4],q[2]={X*pc[3]-Y*pc[2]+120*Q,Q},u=(dx*pc[2]+dz*pc[3])/pc[4],v=dx*pc[3]-dz*pc[2]+120*u;
    double Lo=ga-g0,Hi=gb-g0;
    lim(&Lo,&Hi,q[1]-.02,u);lim(&Lo,&Hi,zp-q[1],-u);lim(&Lo,&Hi,q[0]+40*q[1],v+40*u);lim(&Lo,&Hi,280*q[1]-q[0],280*u-v);
    if(!(Lo<Hi))continue;
    ref_drawn_chords++;
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
      double t=Z>k*U/.3?Z:k*U/.3; double g=U>1e-7?(t-q[1])/U:-1e9; if(n0>g)g=n0;
      const double hh=k>s?ceil(g/k)*k:n0,c=jr((e-hh)/k),z=q[1]+e*U;
      if(c>0){const double in[8]={q[0]+e*V,-V*k,-z,U*k,c>255?255:c,1/z,c0,c1};rdraw(turf?K_PT:K_PRAIL,in);}
      if(k==s)break;
      if(hh<e)e=hh;k/=2;
    }
    if(!turf&&b<0){
      const double y=q[1]+Lo*u,z=q[1]+Hi*u,xa=(q[0]+Lo*v)/y,xb=(q[0]+Hi*v)/z;
      const double h[8]={xa,1/y,xb,1/z,6,-2.4,hl_lines,21130},c[8]={xa,28+4.8/y,xb,28+4.8/z,-2.4/y,-2.4/z,Lo*cells,Hi*cells};
      rdraw(K_HL,h);rdraw(K_CROWD,c);
    }
  }
}
// ---- fixed point. Positions Q16 m (int32), directions Q30, f Q16 px, 1/f Q32.
// Per-series chord cache (frame independent), filled in double at registration.
typedef struct{i64 ga,gb,ax,az,tx,tz,nx,nz;}qchord;
static qchord QC[8][32]; // series slot, chord
static double key[8][3]; static int nkey; static bool key_oval[8];
static i64 rq(double v,int q){return (i64)llround(v*ldexp(1,q));}
static int series_slot(double w,double s,double g0){
  for(int k=0;k<nkey;k++)if(key[k][0]==w&&key[k][1]==s&&key[k][2]==g0)return k;
  int k=nkey<8?nkey++:0; key[k][0]=w;key[k][1]=s;key[k][2]=g0;
  for(int j=0;j+1<nends;j++){
    const double ga=g0+jr((ch[j]-g0)/s)*s,gb=g0+jr((ch[j+1]-g0)/s)*s;
    double A[4],B[4];pose(ga,w,A);pose(gb,w,B);
    const double dx=(B[0]-A[0])/(gb-ga),dz=(B[1]-A[1])/(gb-ga),l=sqrt(dx*dx+dz*dz);
    qchord*c=&QC[k][j];
    c->ga=rq(ga,16);c->gb=rq(gb,16);c->ax=rq(A[0],16);c->az=rq(A[1],16);c->tx=rq(dx,30);c->tz=rq(dz,30);c->nx=rq(-dz/l,30);c->nz=rq(dx/l,30);
  }
  return k;
}
static long ndiv64,nsqrt,ndiv32,nmul,int_chords;
static i64 m30(i64 a,i64 b){nmul++;return (a*b+(1LL<<29))>>30;}
static i64 fdiv(i64 a,i64 b){ndiv32++;i64 q=a/b;if(a%b&&((a<0)!=(b<0)))q--;return q;}
static i64 cdiv(i64 a,i64 b){return -fdiv(-a,b);}
static i64 div64(i64 a,i64 b){ndiv64++;return a/b;}
static i64 isqrt64(i64 v){nsqrt++;if(v<=0)return 0;i64 r=(i64)sqrt((double)v);while(r*r>v)r--;while((r+1)*(r+1)<=v)r++;return r;}
static i64 Ex,Ez,Dx,Dz,Fq,INVF,ZFq,ZN; // camera in fixed
static void cam_fixed(void){Ex=rq(pc[0],16);Ez=rq(pc[1],16);Dx=rq(pc[2],30);Dz=rq(pc[3],30);Fq=rq(pc[4],16);INVF=rq(1/pc[4],32);ZFq=rq(zf,16);ZN=Fq/50;}
typedef struct{i64 Zq,Xq,uz,ux;}line_t; // Z(G)=Zq+G uz, X(G)=Xq+G ux (G Q16 m from g0)
static i64 Zat(const line_t*l,i64 G){return l->Zq+m30(G,l->uz);}
static i64 Xat(const line_t*l,i64 G){return l->Xq+m30(G,l->ux);}
// plane value (Q16-ish, any positive scale): 0 near, 1 far, 2 left (x>=-40), 3 right (x<=280)
static i64 plane(int p,i64 Z,i64 X){nmul++;const i64 fx=(Fq*X)>>16;return p==0?Z-ZN:p==1?ZFq-Z:p==2?fx+160*Z:160*Z-fx;}
static bool inrq(const line_t*l,i64 G){const i64 Z=Zat(l,G),X=Xat(l,G),fx=(Fq*X)>>16;nmul++;return Z>ZN&&fx+520*Z>0&&520*Z-fx>0;}
static void ser_int(bool turf,double w,double s_,double g0_,int L,double a_,double b_){
  const int slot=series_slot(w,s_,g0_);
  const i64 s=rq(s_,16),g0=rq(g0_,16);
  for(int i=0;i<in_view;i++){
    const int j=vc[i];
    if(nsec&&ou(j,w,s_,zf))continue; // culling stays as it is in this experiment
    int_chords++;
    const qchord*c=&QC[slot][j];
    const i64 rx=c->ax-Ex,rz=c->az-Ez;
    const i64 Za=(rx*Dx+rz*Dz+(1LL<<29))>>30,Xa=(rx*Dz-rz*Dx+(1LL<<29))>>30;nmul+=4;
    line_t l;l.uz=(c->tx*Dx+c->tz*Dz+(1LL<<29))>>30;l.ux=(c->tx*Dz-c->tz*Dx+(1LL<<29))>>30;nmul+=4;
    const i64 go=c->ga-g0;
    l.Zq=Za-m30(go,l.uz);l.Xq=Xa-m30(go,l.ux);
    i64 lo=go,hi=c->gb-g0;
    bool out=false;
    for(int p=0;p<4&&!out;p++){
      i64 A=plane(p,Zat(&l,lo),Xat(&l,lo)),B=plane(p,Zat(&l,hi),Xat(&l,hi));
      if(A<0&&B<0){out=true;break;}
      if(A>=0&&B>=0)continue;
      while(llabs(A)>=(1LL<<30)||llabs(B)>=(1LL<<30))A>>=1,B>>=1; // nsau on the device
      if(A==B){out=true;break;}
      const i64 cut=lo+div64((hi-lo)*A,A-B);
      if(A<0)lo=cut;else hi=cut;
    }
    if(out||!(lo<hi))continue;
    const int o=l.uz<0?-1:1;
    line_t f={l.Zq,l.Xq,l.uz*o,l.ux*o}; // in G' = o G: depth rises with G'
    const i64 U=f.uz,t1=o>0?hi:-lo,ne=o>0?lo:-hi;
    i64 W=llabs(l.ux*l.Zq-l.Xq*l.uz)>>30;nmul+=2;if(W>(1LL<<26))W=1LL<<26;
    i64 zsq=((Fq*W)>>16)*s;nmul+=2;
    zsq=(L==4)?zsq>>2:(L==8)?zsq>>3:zsq/L;
    i64 Zl=isqrt64(zsq),k=s,e,n0;
    const i64 Zfar=Zat(&f,t1);
    while(Zl<Zfar&&2*m30(k,U)<((Zfar*19661)>>16)&&k<64*s){k*=2;Zl=m30(Zl,1518500250);} // sqrt(2) Q30
    e=cdiv(t1,k)*k; if(!inrq(&f,e))e-=k;
    n0=fdiv(ne,s)*s; if(!inrq(&f,n0))n0+=s;
    i64 c0=turf?0:(i64)a_,c1=turf?0:rq(b_,16);
    if(turf){
      const i64 a=rq(a_,16),nd=(c->nx*Dx+c->nz*Dz+(1LL<<29))>>30,np=(c->nx*Dz-c->nz*Dx+(1LL<<29))>>30;nmul+=4;
      const i64 dZ=m30(a,nd),dX=m30(a,np);
      c1=-((dZ*INVF)>>24);nmul++;      // Q24
      c0=dX-((120*c1)>>8);             // Q16
    }
    for(;;){
      Zl=m30(Zl,759250125); // 1/sqrt(2) Q30
      i64 hh=n0;
      if(k>s){
        i64 t=m30(k,U)*10/3; if(Zl>t)t=Zl;
        i64 g;
        if(U>107){ // 1e-7 Q30
          const i64 d=t-f.Zq; // Q16; G = d / U
          g=(llabs(d)>>31)>=U? (d<0?-(1LL<<40):(1LL<<40)) : div64(d*(1LL<<30),U);
        }else g=-(1LL<<40);
        if(n0>g)g=n0;
        hh=cdiv(g,k)*k;
      }
      const i64 cnt=fdiv(e-hh,k);
      if(cnt>0){
        const i64 Ze=Zat(&f,e),Xe=Xat(&f,e);
        const i64 zq24=(Ze*INVF)>>24;nmul++;
        const i64 ku24=(k*U)>>22,dz24=(ku24*INVF)>>32;nmul+=2;
        const i64 kx16=m30(k,f.ux);
        const i64 in[8]={Xe+((120*zq24)>>8),-(kx16+((120*dz24)>>8)),-zq24,dz24,cnt>255?255:cnt,div64(Fq<<16,Ze),c0,c1};
        idraw(turf?K_PT:K_PRAIL,in);
      }
      if(k==s)break;
      if(hh<e)e=hh;k/=2;
    }
    if(!turf&&b_<0){
      const i64 Z0=Zat(&l,lo),X0=Xat(&l,lo),Z1=Zat(&l,hi),X1=Xat(&l,hi);
      const i64 r0=div64(Fq<<16,Z0),r1=div64(Fq<<16,Z1),xa=(120LL<<16)+div64(Fq*X0,Z0),xb=(120LL<<16)+div64(Fq*X1,Z1);
      const i64 h[8]={xa,r0,xb,r1,6<<16,-157286,(i64)hl_lines,21130};
      const i64 cl=rq(cells,16);
      const i64 cr[8]={xa,(28<<16)+((r0*314573)>>16),xb,(28<<16)+((r1*314573)>>16),-((r0*157286)>>16),-((r1*157286)>>16),(lo*cl)>>16,(hi*cl)>>16};
      nmul+=6;
      idraw(K_HL,h);idraw(K_CROWD,cr);
    }
  }
}
// ---- compare: the points each draw puts on the screen
typedef struct{double x,y;}pt_t;
static int points(const draw_t*D,int n,pt_t*P,int max){
  int m=0;
  for(int i=0;i<n;i++){
    const double*in=D[i].in;
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
      P[m++]=(pt_t){in[0],28+(6-2.4*4)*in[1]};P[m++]=(pt_t){in[2],28+(6-2.4*4)*in[3]};
    }
  }
  return m;
}
static long hist[6],npts,unmatched_on; static double worst; static long calls,draw_diff,cnt_ref,cnt_int;
static bool on(pt_t p){return p.x>=0&&p.x<=240&&p.y>=-20&&p.y<=155;}
static void match(const pt_t*A,int na,const pt_t*B,int nb){
  for(int i=0;i<na;i++){
    if(!on(A[i]))continue;
    double best=1e9;
    for(int j=0;j<nb;j++){double d=fmax(fabs(A[i].x-B[j].x),fabs(A[i].y-B[j].y));if(d<best)best=d;}
    npts++;
    hist[best<=.01?0:best<=.1?1:best<=.5?2:best<=1?3:best<=2?4:5]++;
    if(best>worst)worst=best;
    if(best>2)unmatched_on++;
  }
}
static pt_t PA[8192],PB[8192];
static int verbose;
static void compare(const char*tag){
  calls++;cnt_ref+=rn;cnt_int+=in_;if(rn!=in_)draw_diff++;
  int na=points(RD,rn,PA,8192),nb=points(ID,in_,PB,8192);
  double w0=worst;
  match(PA,na,PB,nb);match(PB,nb,PA,na);
  if(verbose&&worst>w0&&worst>2)printf("  worst %.2f at %s call %ld: ref %d draws, int %d draws f=%.1f\n",worst,tag,calls,rn,in_,pc[4]);
}
static void one(const char*tag,bool turf,double w,double s,double g0,int L,double a,double b){
  rn=in_=0;ser_ref(turf,w,s,g0,L,a,b);ser_int(turf,w,s,g0,L,a,b);compare(tag);
}
static uint32_t rs=12345; static double rnd(void){rs^=rs<<13;rs^=rs>>17;rs^=rs<<5;return rs/4294967296.0;}
static const double KN[3][2]={{8,10},{5,7},{4,5}};
static void frame(int tier,int L){
  nkey=nkey; // cache persists per course
  one("stands",false,40,12,0,L,31727,-7.5);
  one("outer",false,1481114/QM,KN[tier][0],0,L,0xad55,321126/QM);
  one("turf0",true,11,2*KN[tier][1],0,L,760218/QM,0);
  one("turf1",true,11,2*KN[tier][1],KN[tier][1],L,760218/QM,0);
  one("inner",false,11,KN[tier][0],0,L,0xffff,321126/QM);
}
static void report(const char*name){
  printf("%s: calls %ld, draws ref %ld int %ld (calls with a different draw count %ld), saturated inputs %ld\n",name,calls,cnt_ref,cnt_int,draw_diff,sat_events);
  printf("  on-screen points %ld: <=0.01 px %ld, <=0.1 %ld, <=0.5 %ld, <=1 %ld, <=2 %ld, >2 %ld; worst %.4f px\n",npts,hist[0],hist[1],hist[2],hist[3],hist[4],hist[5],worst);
  printf("  per chord (int): mul64 %.1f, div32 %.2f, div64 %.2f, isqrt %.2f; chords ref %ld (drawn %ld) int %ld\n",(double)nmul/int_chords,(double)ndiv32/int_chords,(double)ndiv64/int_chords,(double)nsqrt/int_chords,ref_chords,ref_drawn_chords,int_chords);
  memset(hist,0,sizeof hist);npts=unmatched_on=calls=draw_diff=cnt_ref=cnt_int=sat_events=0;worst=0;nmul=ndiv32=ndiv64=nsqrt=int_chords=ref_chords=ref_drawn_chords=0;
}
int main(int argc,char**argv){
  verbose=argc>1;
  // 1. the app's cameras: three units, the leader along the course, f 200..1500 by the leader's distance
  for(int oval=0;oval<2;oval++){
    course(oval);nkey=0;
    for(int tier=0;tier<3;tier++){nkey=0;
      for(int u=0;u<3;u++)for(double g=-20;g<=1010;g+=0.37){
        const double ug[3]={100,460,820},uw=oval&&u==1?-3:-14;
        aim(ug[u],uw,g,200,14,1500);view(tier?75:150);cam_fixed();
        frame(tier,((int)(g*7))&1?8:4);
      }
    }
    report(oval?"app cameras, oval":"app cameras, straight");
  }
  // 2. random cameras (as __serfuzz, L != 0)
  for(int n=0;n<40000;n++){
    const bool oval=rnd()<.7;course(oval);nkey=0;
    const int tier=(int)(rnd()*3);
    const double g=-150+1300*rnd();double gc,wc,f;
    switch((int)(rnd()*5)){case 0:gc=g;break;case 1:gc=g-40+80*rnd();break;case 2:gc=100+900*rnd();break;case 3:gc=460;break;default:gc=820;}
    switch((int)(rnd()*5)){case 0:wc=-14;break;case 1:wc=-100;break;case 2:wc=-3;break;case 3:wc=-150+210*rnd();break;default:wc=11+30*rnd();}
    switch((int)(rnd()*4)){case 0:f=200;break;case 1:f=1500;break;case 2:f=0;break;default:f=50+(int)(3000*rnd());}
    if(f)aim(gc,wc,g,f,14,f);else aim(gc,wc,g,200,14,1500);
    const double ex[3]={75,150,(int)(500*rnd())};view(ex[(int)(rnd()*3)]);cam_fixed();
    frame(tier,rnd()<.5?4:8);
    for(int i=0;i<3;i++){
      const double ss[10]={1,2.5,4,5,7,8,10,12,14,16*rnd()+.5},s=ss[(int)(rnd()*10)],g0s[3]={0,s/2,s*rnd()},g0=g0s[(int)(rnd()*3)];
      const double w=5+45*rnd();const int bi=(int)(rnd()*5);
      // Q16 arguments, as the app passes them
      const double sq=floor(s*65536)/65536,g0q=floor(g0*65536)/65536,wq=floor(w*65536)/65536;
      if(bi==4)one("fz-turf",true,wq,sq,g0q,rnd()<.5?4:8,floor((5+20*rnd())*65536)/65536,0);
      else{const double bs[4]={-7.5,4.9,-1-9*rnd(),9*rnd()};one("fz-rail",false,wq,sq,g0q,rnd()<.5?4:8,31727,floor(bs[bi]*65536)/65536);}
    }
  }
  report("random cameras (fuzz)");
  return 0;
}
