#include "ksn_shiki_frame.h"
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

int g_ksn_proc_hspan=1;
/* Columns a..b (inclusive, already on the panel) of one row in colour c, two
 * pixels a 32-bit store once the address is 4-aligned. may_alias: the rest of
 * the renderer reads and writes these pixels as uint16_t
 * (docs/vm/aliasing-types.md). */
static inline __attribute__((always_inline)) void fill_row(uint16_t *row,int a,int b,uint16_t c){
    typedef uint32_t pixel_pair __attribute__((may_alias));
    uint16_t *p=row+a;
    unsigned n=(unsigned)(b-a+1);
    if(((uintptr_t)p&2u)){*p++=c;n--;}
    pixel_pair *w=(pixel_pair *)(void *)p;
    const uint32_t v=(uint32_t)c|(uint32_t)c<<16;
    for(unsigned i=n/2;i;i--)*w++=v;
    if(n&1u)p[n-1]=c;
}
/* A horizontal line in the band (the caller's range test put its row there):
 * the Bresenham walk of dy = 0 writes exactly the row's columns between the
 * ends, so clip them to the panel and fill. */
static inline __attribute__((always_inline)) void hspan(const ksn_proc_segment *s,int x0,
                                                        uint16_t *pixels,int y){
    int a=x0<s->x1?x0:s->x1,b=x0<s->x1?s->x1:x0;
    if(a<0)a=0;
    if(b>=KSN_PROC_W)b=KSN_PROC_W-1;
    if(a<=b)fill_row(pixels+(s->y0-y)*KSN_PROC_W,a,b,s->color);
}
/* The Bresenham phase where line s (pen end x0) reaches band y..y+height-1
 * mid-way: x, yy, err moved there; returns the major-axis steps taken (the
 * pixel index), 0 when the line starts in the band. */
static inline __attribute__((always_inline)) int band_entry(const ksn_proc_segment *s,int x0,int y,int height,
                                                            int *x,int *yy,int *err){
    const int dx=abs(s->x1-x0),dy=abs(s->y1-s->y0),sx=x0<s->x1?1:-1,sy=s->y0<s->y1?1:-1;
    const int row=sy>0?y-s->y0:s->y0-(y+height-1);
    if(!dy||row<=0)return 0;
    int major,minor,x_steps,y_steps;
    /* int16 endpoints and a panel band bound row by 32768 + KSN_PROC_H.
     * The positive division numerators fit uint32_t, including rounding. */
    if(dx>=dy){
        uint32_t numerator=(uint32_t)row*dx-(dx-1)/2;
        major=numerator>0?(int)((numerator+dy-1)/(unsigned)dy):0;
        minor=(int)(((uint32_t)major*dy+(dx-1)/2)/(unsigned)dx);
        x_steps=major;y_steps=minor;
    }else{
        major=row;
        minor=(int)(((uint32_t)major*dx+(dy-1)/2)/(unsigned)dy);
        x_steps=minor;y_steps=major;
    }
    *x+=sx*x_steps;*yy+=sy*y_steps;
    *err=(int)((int64_t)dx-dy-(int64_t)x_steps*dy+(int64_t)y_steps*dx);
    return major;
}
/* A plain line in a frame with pattern lines: ksn_proc_render_band's walk. */
static inline __attribute__((always_inline)) void plain_band(const ksn_proc_segment *s,uint16_t *pixels,int y,int height){
    if(!height)return;
    if(g_ksn_proc_hspan&&s->y0==s->y1){hspan(s,s->x0,pixels,y);return;}
    int x=s->x0,yy=s->y0,dx=abs(s->x1-s->x0),dy=abs(s->y1-s->y0);
    int sx=x<s->x1?1:-1,sy=yy<s->y1?1:-1,err=dx-dy;
    band_entry(s,s->x0,y,height,&x,&yy,&err);
    for(;;){
        if(x>=0&&x<KSN_PROC_W)pixels[(yy-y)*KSN_PROC_W+x]=s->color;
        if(x==s->x1&&yy==s->y1)break;
        int twice=2*err;
        if(twice>-dy){err-=dy;x+=sx;}
        if(twice<dx){err+=dx;yy+=sy;
            if((sy>0&&yy>=y+height)||(sy<0&&yy<y))break;
        }
    }
}
/* One pixel of a pattern line: cell u >> 8 of the pattern. */
#define PATTERN_PIXEL(at) do{ if(bits>>(u>>8)&1u)(at)=ca; else if(has_b)(at)=cb; }while(0)
/* A pattern line's pixels inside the band: g its geometry, q its parameters.
 * The walk is the plain line's, with the pixel index k carried as u. A
 * horizontal line (a stand's row from the side) clips and runs left to
 * right; a slanted one keeps its row's address and tests only the major
 * axis's end (that axis moves every step). */
static void pattern_band(const ksn_proc_segment *g,const ksn_proc_segment *q,
                         uint16_t *pixels,int y,int height){
    const unsigned n=(uint16_t)q->y0>>8&31u;
    if(n<1||n>KSN_PROC_PATTERN_BITS)return;
    const uint32_t wrap=n*256u,u0=(uint16_t)q->x1%wrap,du=((uint16_t)q->y1&0x7fffu)%wrap;
    const uint32_t bits=(uint16_t)q->x0|(uint32_t)((uint16_t)q->y0&0xffu)<<16;
    const bool has_b=(uint16_t)q->y0>>13&1u;
    const uint16_t ca=g->color,cb=q->color;
    const int x0=g->x0-KSN_PROC_EXT_PATTERN;
    if(g->y0==g->y1){
        const bool forward=x0<=g->x1;
        int left=forward?x0:g->x1,right=forward?g->x1:x0;
        if(left<0)left=0;
        if(right>=KSN_PROC_W)right=KSN_PROC_W-1;
        if(left>right)return;
        /* k counts from the pen, whichever side that is; left to right is
         * backwards (wrap - du) when the pen is on the right. */
        const uint32_t k=(uint32_t)(forward?left-x0:x0-left);
        const uint32_t step=forward?du:(wrap-du)%wrap;
        /* 32 bits hold it: u0 and du are under 2^13, k at most 1,200. */
        uint32_t u=(u0+k*du)%wrap;
        uint16_t *row=pixels+(g->y0-y)*KSN_PROC_W;
        for(int x=left;x<=right;x++){
            PATTERN_PIXEL(row[x]);
            u+=step;if(u>=wrap)u-=wrap;
        }
        return;
    }
    const int dx=abs(g->x1-x0),dy=abs(g->y1-g->y0),sx=x0<g->x1?1:-1,sy=g->y0<g->y1?1:-1;
    int x=x0,yy=g->y0,err=dx-dy;
    const uint32_t k=(uint32_t)band_entry(g,x0,y,height,&x,&yy,&err);
    /* The range test let the line in, so its entry point is in the band. */
    uint32_t u=(u0+k*du)%wrap;
    uint16_t *row=pixels+(yy-y)*KSN_PROC_W;
    const int stride=sy*KSN_PROC_W;
    if(dx>=dy){
        for(;;){
            if((unsigned)x<(unsigned)KSN_PROC_W)PATTERN_PIXEL(row[x]);
            if(x==g->x1)break;
            const int twice=2*err;
            if(twice>-dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;yy+=sy;if(yy<y||yy>=y+height)break;row+=stride;}
            u+=du;if(u>=wrap)u-=wrap;
        }
    }else{
        for(;;){
            if((unsigned)x<(unsigned)KSN_PROC_W)PATTERN_PIXEL(row[x]);
            if(yy==g->y1)break;
            const int twice=2*err;
            if(twice>-dy){err-=dy;x+=sx;}
            if(twice<dx){err+=dx;yy+=sy;}
            if(yy<y||yy>=y+height)break;
            row+=stride;
            u+=du;if(u>=wrap)u-=wrap;
        }
    }
}
#undef PATTERN_PIXEL
static bool ell_finite(const float *p,unsigned n,float limit){
    for(unsigned i=0;i<n;i++)if(!isfinite(p[i])||fabsf(p[i])>limit)return false;
    return true;
}
static bool ell_unit(float v[3]){
    const float s=fmaxf(fabsf(v[0]),fmaxf(fabsf(v[1]),fabsf(v[2])));
    if(!(s>0))return false;
    v[0]/=s;v[1]/=s;v[2]/=s;
    const float d=sqrtf(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
    for(unsigned i=0;i<3;i++)v[i]/=d;
    return true;
}
bool ksn_proc_ellipsoid_bounds(const ksn_proc_ellipsoid *p,int *left,int *top,int *right,int *bottom){
    if(!p||!ell_finite(p->screen,8,4096)||!ell_finite(p->normal,9,1e6f)||
       !ell_finite(p->light,9,1)||!ell_finite(p->material,8,128)||
       !ell_finite(p->texture,9,4096)||p->surface[2]>255||p->surface[3]>255||p->surface[4]>4||p->reserved)return false;
    if(p->surface[4]==4){
        const float *t=p->texture;
        const double n2=(double)t[0]*t[0]+(double)t[1]*t[1]+(double)t[2]*t[2];
        if(fabs(n2-1)>1e-4||fabsf(t[3])>2||t[4]<0||t[4]>2||
           (t[4]>0&&t[4]<1e-4f)||t[5]!=0||t[7]!=0||t[8]!=1)return false;
    }
    const float *a=p->screen,*b=p->screen+4,*n=p->normal;
    const float aa=a[0]*a[0]+a[1]*a[1]+a[2]*a[2],bb=b[0]*b[0]+b[1]*b[1]+b[2]*b[2],
        ab=a[0]*b[0]+a[1]*b[1]+a[2]*b[2],det=aa*bb-ab*ab;
    const float nd=n[0]*(n[4]*n[8]-n[5]*n[7])-n[1]*(n[3]*n[8]-n[5]*n[6])+n[2]*(n[3]*n[7]-n[4]*n[6]);
    if(!(aa>0&&bb>0&&det>aa*bb*1e-6f)||!isfinite(nd)||nd==0)return false;
    float l[3]={p->light[0],p->light[1],p->light[2]},v[3]={p->light[3],p->light[4],p->light[5]};
    if(!ell_unit(l)||!ell_unit(v))return false;
    for(unsigned i=6;i<9;i++)if(p->light[i]<0)return false;
    for(unsigned i=0;i<8;i++)if(i==3?(p->material[i]<1||p->material[i]>128):
        (p->material[i]<0||p->material[i]>1))return false;
    if(p->texture[6]<0||p->texture[6]>1||p->texture[7]<0||p->texture[8]>1||p->texture[7]>p->texture[8])return false;
    const float rx=sqrtf(aa),ry=sqrtf(bb);
    *left=(int)ceilf(a[3]-rx);*right=(int)floorf(a[3]+rx);
    *top=(int)ceilf(b[3]-ry);*bottom=(int)floorf(b[3]+ry);
    if(*left<0)*left=0;
    if(*top<0)*top=0;
    if(*right>=KSN_PROC_W)*right=KSN_PROC_W-1;
    if(*bottom>=KSN_PROC_H)*bottom=KSN_PROC_H-1;
    if(!p->surface[2]){*left=1;*right=0;}
    return true;
}
static bool clip_prepare(const ksn_proc_ellipsoid_clip *p,int *winding){
    if(p->reserved[0]||p->reserved[1]||p->reserved[2]||
       (p->count!=0&&(p->count<3||p->count>KSN_PROC_CLIP_VERTICES)))return false;
    for(unsigned k=0;k<2u*p->count;k++)if(!isfinite(p->xy[k])||fabsf(p->xy[k])>4096)return false;
    for(unsigned k=2u*p->count;k<16;k++){uint32_t bits;memcpy(&bits,p->xy+k,4);if(bits)return false;}
    if(!p->count){*winding=0;return true;}
    int orientation=0;
    for(unsigned i=0;i<p->count;i++){
        const unsigned next=(i+1u)%p->count;
        const double ax=p->xy[2*i],ay=p->xy[2*i+1],dx=(double)p->xy[2*next]-ax,dy=(double)p->xy[2*next+1]-ay;
        if(dx==0&&dy==0)return false;
        /* Checking all vertices, not just adjacent turns, rejects stars. */
        for(unsigned j=0;j<p->count;j++)if(j!=i&&j!=next){
            const double cross=dx*((double)p->xy[2*j+1]-ay)-dy*((double)p->xy[2*j]-ax);
            const int sign=(cross>0)-(cross<0);
            if(!sign||(orientation&&sign!=orientation))return false;
            orientation=sign;
        }
    }
    *winding=orientation;return true;
}
static bool clip_contains(const ksn_proc_ellipsoid_clip *p,int winding,int x,int y){
    for(unsigned i=0;i<p->count;i++){
        const unsigned next=(i+1u)%p->count;
        const double ax=p->xy[2*i],ay=p->xy[2*i+1],dx=(double)p->xy[2*next]-ax,dy=(double)p->xy[2*next+1]-ay;
        if(winding*(dx*((double)y-ay)-dy*((double)x-ax))<0)return false;
    }
    return p->count!=0;
}
/* Closed integer pixel centres. Double intersection work is per row, not
 * per shaded pixel; endpoint halfplanes repair division rounding at ties. */
static bool clip_span(const ksn_proc_ellipsoid_clip *p,int winding,int y,int *left,int *right){
    if(!p->count)return false;
    double lo=*left,hi=*right;
    for(unsigned i=0;i<p->count;i++){
        const unsigned next=(i+1u)%p->count;
        const double ax=p->xy[2*i],ay=p->xy[2*i+1],dx=(double)p->xy[2*next]-ax,dy=(double)p->xy[2*next+1]-ay;
        if(dy==0){if(winding*dx*((double)y-ay)<0)return false;continue;}
        const double intersection=ax+((double)y-ay)*dx/dy;
        if(winding*dy>0)hi=fmin(hi,intersection);else lo=fmax(lo,intersection);
    }
    const double first=fmax(*left,ceil(lo)-1),last=fmin(*right,floor(hi)+1);
    if(first>last||first>*right||last<*left)return false;
    int a=(int)first,b=(int)last;
    while(a<=b&&!clip_contains(p,winding,a,y))++a;
    while(b>=a&&!clip_contains(p,winding,b,y))--b;
    if(a>b)return false;
    *left=a;*right=b;return true;
}
bool ksn_proc_clipped_ellipsoid_read(const ksn_proc_segment *data,unsigned words,ksn_proc_clipped_ellipsoid *out){
    if(!data||!out||words<KSN_PROC_CLIPPED_WORDS)return false;
    const uint8_t *raw=(const uint8_t *)data;
    for(size_t k=sizeof *out;k<KSN_PROC_CLIPPED_WORDS*sizeof *data;k++)if(raw[k])return false;
    memcpy(out,data,sizeof *out);return true;
}
bool ksn_proc_clipped_ellipsoid_bounds(const ksn_proc_clipped_ellipsoid *p,int *left,int *top,int *right,int *bottom){
    int l,t,r,b,winding;
    /* EMPTY never hides invalid analytic geometry. */
    if(!p||!ksn_proc_ellipsoid_bounds(&p->patch,&l,&t,&r,&b)||!clip_prepare(&p->clip,&winding))return false;
    if(!p->clip.count){l=1;r=0;}
    else{
        float x0=p->clip.xy[0],x1=x0,y0=p->clip.xy[1],y1=y0;
        for(unsigned k=1;k<p->clip.count;k++){
            x0=fminf(x0,p->clip.xy[2*k]);x1=fmaxf(x1,p->clip.xy[2*k]);
            y0=fminf(y0,p->clip.xy[2*k+1]);y1=fmaxf(y1,p->clip.xy[2*k+1]);
        }
        if(ceilf(x0)>l)l=(int)ceilf(x0);
        if(floorf(x1)<r)r=(int)floorf(x1);
        if(ceilf(y0)>t)t=(int)ceilf(y0);
        if(floorf(y1)<b)b=(int)floorf(y1);
    }
    *left=l;*top=t;*right=r;*bottom=b;return true;
}
static bool gouraud_zero(const void *data,size_t bytes){
    const uint8_t *p=data;for(size_t i=0;i<bytes;i++)if(p[i])return false;return true;
}
static bool gouraud_header(const ksn_proc_gouraud_header *h){
    return h->vertices>=3&&h->vertices<=KSN_PROC_GOURAUD_VERTICES&&
        h->triangles>=1&&h->triangles<=KSN_PROC_GOURAUD_TRIANGLES&&
        h->holes<=KSN_PROC_GOURAUD_HOLES&&!h->reserved&&!h->reserved2;
}
bool ksn_proc_gouraud_read(const ksn_proc_segment *data,unsigned words,ksn_proc_gouraud_patch *p,unsigned *used){
    if(!data||!p||!used||words<(sizeof p->header+sizeof *data-1)/sizeof *data)return false;
    ksn_proc_gouraud_header h;memcpy(&h,data,sizeof h);
    if(!gouraud_header(&h))return false;
    const size_t bytes=KSN_PROC_GOURAUD_BYTES(h.vertices,h.triangles,h.holes);
    const unsigned count=(unsigned)KSN_PROC_GOURAUD_WORDS(h.vertices,h.triangles,h.holes);
    if(count>words)return false;
    const uint8_t *raw=(const uint8_t *)data;
    if(!gouraud_zero(raw+bytes,count*sizeof *data-bytes))return false;
    memset(p,0,sizeof *p);p->header=h;size_t offset=sizeof h;
    memcpy(p->vertices,raw+offset,h.vertices*sizeof p->vertices[0]);offset+=h.vertices*sizeof p->vertices[0];
    memcpy(p->indices,raw+offset,3u*h.triangles*sizeof p->indices[0]);offset+=3u*h.triangles*sizeof p->indices[0];
    memcpy(p->clips,raw+offset,h.holes*sizeof p->clips[0]);*used=count;return true;
}
static double gouraud_area(const ksn_proc_gouraud_vertex *a,const ksn_proc_gouraud_vertex *b,
                           const ksn_proc_gouraud_vertex *c){
    return ((double)b->x-a->x)*((double)c->y-a->y)-((double)b->y-a->y)*((double)c->x-a->x);
}
static void gouraud_box(const ksn_proc_gouraud_vertex *a,const ksn_proc_gouraud_vertex *b,
                        const ksn_proc_gouraud_vertex *c,int *left,int *top,int *right,int *bottom){
    *left=(int)fmax(0,ceil(fmin(a->x,fmin(b->x,c->x))));
    *top=(int)fmax(0,ceil(fmin(a->y,fmin(b->y,c->y))));
    *right=(int)fmin(KSN_PROC_W-1,floor(fmax(a->x,fmax(b->x,c->x))));
    *bottom=(int)fmin(KSN_PROC_H-1,floor(fmax(a->y,fmax(b->y,c->y))));
}
bool ksn_proc_gouraud_bounds(const ksn_proc_gouraud_patch *p,int *left,int *top,int *right,int *bottom,unsigned *cost){
    if(!p||!left||!top||!right||!bottom||!cost||!gouraud_header(&p->header))return false;
    const unsigned vertices=p->header.vertices,triangles=p->header.triangles,holes=p->header.holes;
    if(!gouraud_zero(p->vertices+vertices,(KSN_PROC_GOURAUD_VERTICES-vertices)*sizeof p->vertices[0])||
       !gouraud_zero(p->indices+3*triangles,(KSN_PROC_GOURAUD_TRIANGLES-triangles)*3*sizeof p->indices[0])||
       !gouraud_zero(p->clips+holes,(KSN_PROC_GOURAUD_HOLES-holes)*sizeof p->clips[0]))return false;
    for(unsigned i=0;i<vertices;i++){
        const ksn_proc_gouraud_vertex *v=p->vertices+i;
        if(!isfinite(v->x)||!isfinite(v->y)||!isfinite(v->mask)||fabsf(v->x)>4096||
           fabsf(v->y)>4096||fabsf(v->mask)>4096)return false;
    }
    for(unsigned i=0;i<holes;i++){int winding;if(p->clips[i].count<3||!clip_prepare(p->clips+i,&winding))return false;}
    int l=KSN_PROC_W,t=KSN_PROC_H,r=-1,b=-1;unsigned steps=0;
    for(unsigned i=0;i<triangles;i++){
        const uint16_t *index=p->indices+3*i;
        if(index[0]>=vertices||index[1]>=vertices||index[2]>=vertices)return false;
        const ksn_proc_gouraud_vertex *a=p->vertices+index[0],*v=p->vertices+index[1],*c=p->vertices+index[2];
        if(!(gouraud_area(a,v,c)>1e-5))return false;
        int x0,y0,x1,y1;gouraud_box(a,v,c,&x0,&y0,&x1,&y1);
        if(x0>x1||y0>y1)continue;
        steps+=(unsigned)(x1-x0+1)*(unsigned)(y1-y0+1);
        if(x0<l)l=x0;
        if(y0<t)t=y0;
        if(x1>r)r=x1;
        if(y1>b)b=y1;
    }
    *left=l;*top=t;*right=r;*bottom=b;*cost=steps;return true;
}
bool __attribute__((noinline)) ksn_proc_gouraud_frame_next(const ksn_proc_frame *frame,unsigned *i,
                                                         ksn_proc_segment *geometry,unsigned *cost){
    ksn_proc_gouraud_patch p;unsigned used;int l,t,r,b;
    if(!ksn_proc_gouraud_read(frame->segments+*i,frame->count-*i,&p,&used)||
       !ksn_proc_gouraud_bounds(&p,&l,&t,&r,&b,cost)){
        *i=frame->count+1u;return false;
    }
    *i+=used;geometry->x0=(int16_t)(geometry->x0-KSN_PROC_EXT_GOURAUD_PATCH);
    if(l!=geometry->x0||t!=geometry->y0||r!=geometry->x1||b!=geometry->y1||l>r||t>b){
        *i=frame->count+1u;return false;
    }
    return true;
}
typedef struct {double a,b,c;int sign;bool closed;} gouraud_edge;
static gouraud_edge gouraud_make_edge(const ksn_proc_gouraud_vertex *a,const ksn_proc_gouraud_vertex *b){
    const double dx=(double)b->x-a->x,dy=(double)b->y-a->y;
    gouraud_edge edge={.sign=1,.closed=dy<0||(dy==0&&dx>0)};
    /* Both triangles evaluate a shared edge in the same canonical direction.
     * Reversing it only negates the result, including exact zero at ties. */
    if(a->x>b->x||(a->x==b->x&&a->y>b->y)){
        const ksn_proc_gouraud_vertex *swap=a;a=b;b=swap;edge.sign=-1;
    }
    edge.a=(double)a->y-b->y;edge.b=(double)b->x-a->x;
    edge.c=((double)b->y-a->y)*a->x-((double)b->x-a->x)*a->y;return edge;
}
static double gouraud_edge_value(const gouraud_edge *e,int x,int y){return e->sign*(e->a*x+e->b*y+e->c);}
static __attribute__((noinline)) void gouraud_band(const ksn_proc_gouraud_patch *p,uint16_t *pixels,int y,int height){
    int winding[KSN_PROC_GOURAUD_HOLES];
    for(unsigned h=0;h<p->header.holes;h++)if(!clip_prepare(p->clips+h,winding+h))return;
    for(unsigned i=0;i<p->header.triangles;i++){
        const uint16_t *index=p->indices+3*i;
        const ksn_proc_gouraud_vertex *v[3]={p->vertices+index[0],p->vertices+index[1],p->vertices+index[2]};
        int left,top,right,bottom;gouraud_box(v[0],v[1],v[2],&left,&top,&right,&bottom);
        if(top<y)top=y;
        if(bottom>=y+height)bottom=y+height-1;
        if(left>right||top>bottom)continue;
        const double inverse=1/gouraud_area(v[0],v[1],v[2]);
        const gouraud_edge edge[3]={gouraud_make_edge(v[1],v[2]),gouraud_make_edge(v[2],v[0]),gouraud_make_edge(v[0],v[1])};
        for(int yy=top;yy<=bottom;yy++){
            int hl[KSN_PROC_GOURAUD_HOLES],hr[KSN_PROC_GOURAUD_HOLES];
            for(unsigned h=0;h<p->header.holes;h++){
                hl[h]=left;hr[h]=right;
                if(!clip_span(p->clips+h,winding[h],yy,hl+h,hr+h)){hl[h]=1;hr[h]=0;}
            }
            for(int x=left;x<=right;x++){
                bool hidden=false;
                for(unsigned h=0;h<p->header.holes;h++)if(x>=hl[h]&&x<=hr[h]){hidden=true;break;}
                if(hidden)continue;
                double w[3];bool inside=true;
                for(unsigned k=0;k<3;k++){
                    w[k]=gouraud_edge_value(edge+k,x,yy);
                    if(w[k]<0||(w[k]==0&&!edge[k].closed)){inside=false;break;}
                }
                if(!inside)continue;
                for(unsigned k=0;k<3;k++)w[k]*=inverse;
                const bool second=w[0]*v[0]->mask+w[1]*v[1]->mask+w[2]*v[2]->mask>=0;
                const uint16_t a=second?v[0]->color_b:v[0]->color_a,
                    b=second?v[1]->color_b:v[1]->color_a,c=second?v[2]->color_b:v[2]->color_a;
                /* Keep coverage and near-zero material selection in the wide
                 * reference arithmetic; only bounded 5/6-bit colour values
                 * use float32. A tie may differ by one channel code. */
                const float cw[3]={(float)w[0],(float)w[1],(float)w[2]};
                const unsigned red=(unsigned)lroundf(fmaxf(0,fminf(31,cw[0]*(a>>11)+cw[1]*(b>>11)+cw[2]*(c>>11)))),
                    green=(unsigned)lroundf(fmaxf(0,fminf(63,cw[0]*((a>>5)&63)+cw[1]*((b>>5)&63)+cw[2]*((c>>5)&63)))),
                    blue=(unsigned)lroundf(fmaxf(0,fminf(31,cw[0]*(a&31)+cw[1]*(b&31)+cw[2]*(c&31))));
                pixels[(yy-y)*KSN_PROC_W+x]=(uint16_t)((red<<11)|(green<<5)|blue);
            }
        }
    }
}
static __attribute__((noinline)) bool gouraud_render_entry(const ksn_proc_segment *data,unsigned words,
                                                          uint16_t *pixels,int y,int height,unsigned *used){
    ksn_proc_gouraud_patch p;int l,t,r,b;unsigned cost;
    if(!ksn_proc_gouraud_read(data,words,&p,used)||!ksn_proc_gouraud_bounds(&p,&l,&t,&r,&b,&cost))return false;
    if(b>=y&&t<y+height)gouraud_band(&p,pixels,y,height);
    return true;
}
bool ksn_proc_blend_read(const ksn_proc_segment *data,unsigned words,ksn_proc_blend_ellipsoids *p,unsigned *used){
    if(!data||!p||!used||words*sizeof *data<sizeof p->header)return false;
    memcpy(&p->header,data,sizeof p->header);
    if(!p->header.count||p->header.count>KSN_PROC_BLEND_LOBES)return false;
    *used=(unsigned)KSN_PROC_BLEND_WORDS(p->header.count);
    if(*used>words)return false;
    memcpy(p,data,KSN_PROC_BLEND_BYTES(p->header.count));
    return true;
}
bool ksn_proc_blend_region_read(const ksn_proc_segment *data,unsigned words,
        ksn_proc_blend_ellipsoids *p,ksn_proc_blend_region_mask *mask,unsigned *used){
    if(!data||!p||!mask||!used||words*sizeof *data<sizeof p->header)return false;
    memcpy(&p->header,data,sizeof p->header);
    if(!p->header.count||p->header.count>KSN_PROC_BLEND_LOBES)return false;
    const unsigned bytes=(unsigned)KSN_PROC_BLEND_BYTES(p->header.count),
        needed=(unsigned)KSN_PROC_BLEND_REGION_WORDS(p->header.count);
    if(needed>words)return false;
    memcpy(p,data,bytes);memcpy(mask,(const uint8_t *)data+bytes,sizeof *mask);
    *used=needed;return true;
}
bool ksn_proc_blend_bounds(const ksn_proc_blend_ellipsoids *p,int *left,int *top,int *right,int *bottom){
    if(!p||!p->header.count||p->header.count>KSN_PROC_BLEND_LOBES||p->header.reserved||
       p->header.surface[1]>255||p->header.surface[2]>255||
       !ell_finite(p->header.material,11,128)||!ell_finite(p->header.controls,2,128)||
       p->header.controls[0]<0||p->header.controls[1]<0||p->header.controls[1]>1)return false;
    for(unsigned i=8;i<11;i++)if(p->header.material[i]<0||p->header.material[i]>1)return false;
    ksn_proc_ellipsoid q={.texture={1,0,0,0,1,0,0,0,1},.surface={0,0,255,255,0}};
    memcpy(q.light,p->header.light,sizeof q.light);memcpy(q.material,p->header.material,sizeof q.material);
    *left=KSN_PROC_W;*top=KSN_PROC_H;*right=-1;*bottom=-1;
    for(unsigned i=0;i<p->header.count;i++){
        const ksn_proc_blend_lobe *l=&p->lobes[i];
        if(!ell_finite(l->depth,4,4096))return false;
        memcpy(q.screen,l->screen,sizeof q.screen);memcpy(q.normal,l->normal,sizeof q.normal);
        int x0,y0,x1,y1;if(!ksn_proc_ellipsoid_bounds(&q,&x0,&y0,&x1,&y1))return false;
        const float *a=l->screen,*b=a+4,*d=l->depth;
        const float aa=a[0]*a[0]+a[1]*a[1]+a[2]*a[2],bb=b[0]*b[0]+b[1]*b[1]+b[2]*b[2],
            ab=a[0]*b[0]+a[1]*b[1]+a[2]*b[2];
        if(!isfinite(1/(aa*bb-ab*ab)))return false;
        const float volume=(a[1]*b[2]-a[2]*b[1])*d[0]+(a[2]*b[0]-a[0]*b[2])*d[1]+(a[0]*b[1]-a[1]*b[0])*d[2];
        if(!isfinite(volume)||volume==0)return false;
        if(x0>x1||y0>y1)continue;
        if(x0<*left)*left=x0;
        if(y0<*top)*top=y0;
        if(x1>*right)*right=x1;
        if(y1>*bottom)*bottom=y1;
    }
    if(!p->header.surface[1]){*left=1;*right=0;}
    return true;
}
bool ksn_proc_blend_region_bounds(const ksn_proc_blend_ellipsoids *p,const ksn_proc_blend_region_mask *mask,
        int *left,int *top,int *right,int *bottom){
    if(!mask||mask->reserved||!ell_finite(mask->plane,4,1e6f)||
       !(mask->plane[0]!=0||mask->plane[1]!=0||mask->plane[2]!=0)||!isfinite(mask->feather)||
       !(mask->feather==0||(mask->feather>=1e-4f&&mask->feather<=4096)))return false;
    return ksn_proc_blend_bounds(p,left,top,right,bottom);
}
static unsigned ell_colour(float base,float light,float tint,float front,float spec,float back,float thin,
                           const float *m,unsigned max){
    float c=base*(m[0]+m[1]*front*light)+light*(m[2]*spec+m[4]*thin*back*tint);
    c=fmaxf(0,fminf(1,c));
    return (unsigned)lroundf(c*max);
}
/* A unit sphere projected by two affine rows. Reconstruct its front surface
 * analytically, so geometry storage does not grow with pixel resolution. */
static void ellipsoid_band(const ksn_proc_ellipsoid *p,const ksn_proc_ellipsoid_clip *clip,int winding,
                           uint16_t *pixels,int y,int height){
    int left,top,right,bottom;
    if(!ksn_proc_ellipsoid_bounds(p,&left,&top,&right,&bottom))return;
    if(top<y)top=y;
    if(bottom>=y+height)bottom=y+height-1;
    if(left>right||top>bottom)return;
    const float *a=p->screen,*b=p->screen+4,*nm=p->normal,*m=p->material,*uv=p->texture;
    const float aa=a[0]*a[0]+a[1]*a[1]+a[2]*a[2],bb=b[0]*b[0]+b[1]*b[1]+b[2]*b[2],
        ab=a[0]*b[0]+a[1]*b[1]+a[2]*b[2],inv=1/(aa*bb-ab*ab);
    float axis[3]={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
    (void)ell_unit(axis);
    float l[3]={p->light[0],p->light[1],p->light[2]},v[3]={p->light[3],p->light[4],p->light[5]};
    (void)ell_unit(l);(void)ell_unit(v);
    float sign=0;
    for(unsigned k=0;k<3;k++)sign+=(nm[3*k]*axis[0]+nm[3*k+1]*axis[1]+nm[3*k+2]*axis[2])*v[k];
    if(sign<0)for(unsigned k=0;k<3;k++)axis[k]=-axis[k];
    float half[3]={l[0]+v[0],l[1]+v[1],l[2]+v[2]};const bool has_half=ell_unit(half);
    const float edge=fmaxf(uv[6],2/fminf(sqrtf(aa),sqrtf(bb)));
    const float region_inverse=p->surface[4]==4&&uv[4]>0?1/uv[4]:0;
    for(int yy=top;yy<=bottom;yy++){
      int row_left=left,row_right=right;
      if(clip&&!clip_span(clip,winding,yy,&row_left,&row_right))continue;
      for(int x=row_left;x<=row_right;x++){
        const float dx=x-a[3],dy=yy-b[3],u=(bb*dx-ab*dy)*inv,w=(aa*dy-ab*dx)*inv;
        float q[3]={a[0]*u+b[0]*w,a[1]*u+b[1]*w,a[2]*u+b[2]*w};
        const float residual=1-q[0]*q[0]-q[1]*q[1]-q[2]*q[2];
        if(!(residual>0))continue;
        const float chord=sqrtf(residual);
        for(unsigned k=0;k<3;k++)q[k]+=axis[k]*chord;
        float n[3];
        for(unsigned k=0;k<3;k++)n[k]=nm[3*k]*q[0]+nm[3*k+1]*q[1]+nm[3*k+2]*q[2];
        if(!ell_unit(n))continue;
        uint16_t base=p->surface[0];
        float region_mix=0;
        if(p->surface[4]==4){
            /* Body-local albedo only: do not derive markings from lighting or normals. */
            const float distance=uv[0]*q[0]+uv[1]*q[1]+uv[2]*q[2]-uv[3];
            region_mix=uv[4]>0?fmaxf(0,fminf(1,.5f+distance*region_inverse)):(distance>=0?1:0);
            if(region_mix>=1)base=p->surface[1];
        }else if(p->surface[4]){
            const float tu=atan2f(q[0],q[2])*(1.0f/6.283185307179586f)+0.5f,tv=q[1]*0.5f+0.5f;
            const float U=uv[0]*tu+uv[1]*tv+uv[2],V=uv[3]*tu+uv[4]*tv+uv[5];
            bool second;
            if(p->surface[4]==3)second=((int)floorf(U)+(int)floorf(V))&1;
            else{const float t=p->surface[4]==1?U:V,f=t-floorf(t);second=f>=uv[7]&&f<=uv[8];}
            if(second)base=p->surface[1];
        }
        const float dot=n[0]*l[0]+n[1]*l[1]+n[2]*l[2],front=fmaxf(0,dot),back=fmaxf(0,-dot),
            nh=has_half?fmaxf(0,n[0]*half[0]+n[1]*half[1]+n[2]*half[2]):0,
            spec=front>0&&nh>0?powf(nh,m[3]):0,thin=1-p->surface[3]*(1.0f/255)*chord;
        float red=((base>>11)&31)*(1.0f/31),green=((base>>5)&63)*(1.0f/63),blue_base=(base&31)*(1.0f/31);
        if(p->surface[4]==4&&region_mix>0&&region_mix<1){
            const uint16_t second=p->surface[1];
            red+=((((second>>11)&31)*(1.0f/31))-red)*region_mix;
            green+=((((second>>5)&63)*(1.0f/63))-green)*region_mix;
            blue_base+=(((second&31)*(1.0f/31))-blue_base)*region_mix;
        }
        unsigned r=ell_colour(red,p->light[6],m[5],front,spec,back,thin,m,31),
            gg=ell_colour(green,p->light[7],m[6],front,spec,back,thin,m,63),
            blue=ell_colour(blue_base,p->light[8],m[7],front,spec,back,thin,m,31);
        const unsigned alpha=(unsigned)lroundf(p->surface[2]*fminf(1,residual/edge)),ia=255-alpha;
        uint16_t *dest=pixels+(yy-y)*KSN_PROC_W+x;
        r=(r*alpha+((*dest>>11)&31)*ia+127)/255;
        gg=(gg*alpha+((*dest>>5)&63)*ia+127)/255;
        blue=(blue*alpha+(*dest&31)*ia+127)/255;
        *dest=(uint16_t)((r<<11)|(gg<<5)|blue);
      }
    }
}
typedef struct {
    float aa,bb,ab,inv,axis[3],edge;
    int left,top,right,bottom;
} blend_projection;
/* Blend analytic front heights, not independently composited sphere colours.
 * This has bounded work and no depth buffer. The projected silhouette remains
 * the union of the lobes. Support-regularized shading normals suppress grazing
 * cap seams; they approximate, rather than differentiate, the blended height. */
static void blend_ellipsoids_band(const ksn_proc_blend_ellipsoids *p,const ksn_proc_blend_region_mask *region,
        uint16_t *pixels,int y,int height){
    int left,top,right,bottom;if(!ksn_proc_blend_bounds(p,&left,&top,&right,&bottom))return;
    if(top<y)top=y;
    if(bottom>=y+height)bottom=y+height-1;
    if(left>right||top>bottom)return;
    const ksn_proc_blend_header *h=&p->header;
    const float inverse_feather=region&&region->feather>0?1/region->feather:0;
    const float *m=h->material;
    float light[3]={h->light[0],h->light[1],h->light[2]},view[3]={h->light[3],h->light[4],h->light[5]};
    (void)ell_unit(light);(void)ell_unit(view);
    float half[3]={light[0]+view[0],light[1]+view[1],light[2]+view[2]};const bool has_half=ell_unit(half);
    blend_projection projection[KSN_PROC_BLEND_LOBES];
    for(unsigned i=0;i<h->count;i++){
        const float *a=p->lobes[i].screen,*b=a+4,*d=p->lobes[i].depth;
        blend_projection *s=&projection[i];
        s->aa=a[0]*a[0]+a[1]*a[1]+a[2]*a[2];s->bb=b[0]*b[0]+b[1]*b[1]+b[2]*b[2];
        s->ab=a[0]*b[0]+a[1]*b[1]+a[2]*b[2];s->inv=1/(s->aa*s->bb-s->ab*s->ab);
        s->axis[0]=a[1]*b[2]-a[2]*b[1];s->axis[1]=a[2]*b[0]-a[0]*b[2];s->axis[2]=a[0]*b[1]-a[1]*b[0];
        (void)ell_unit(s->axis);
        if(d[0]*s->axis[0]+d[1]*s->axis[1]+d[2]*s->axis[2]<0)
            for(unsigned k=0;k<3;k++)s->axis[k]=-s->axis[k];
        const float rx=sqrtf(s->aa),ry=sqrtf(s->bb);
        s->left=(int)ceilf(a[3]-rx);s->right=(int)floorf(a[3]+rx);
        s->top=(int)ceilf(b[3]-ry);s->bottom=(int)floorf(b[3]+ry);
        s->edge=fmaxf(h->controls[1],2/fminf(rx,ry));
    }
    for(int yy=top;yy<=bottom;yy++)for(int x=left;x<=right;x++){
        bool found=false;float depth=0,normal[3]={0},chord=0,mass=0,coverage=0;
        for(unsigned i=0;i<h->count;i++){
            const blend_projection *s=&projection[i];
            if(x<s->left||x>s->right||yy<s->top||yy>s->bottom)continue;
            const ksn_proc_blend_lobe *l=&p->lobes[i];const float *a=l->screen,*b=a+4;
            const float dx=x-a[3],dy=yy-b[3],u=(s->bb*dx-s->ab*dy)*s->inv,w=(s->aa*dy-s->ab*dx)*s->inv;
            float q[3]={a[0]*u+b[0]*w,a[1]*u+b[1]*w,a[2]*u+b[2]*w};
            const float residual=1-q[0]*q[0]-q[1]*q[1]-q[2]*q[2];
            if(!(residual>0))continue;
            const float support=h->controls[0]>0&&h->count>1?residual:1;
            const float z=sqrtf(residual);
            for(unsigned k=0;k<3;k++)q[k]+=s->axis[k]*z;
            float n[3];
            for(unsigned k=0;k<3;k++)n[k]=l->normal[3*k]*q[0]+l->normal[3*k+1]*q[1]+l->normal[3*k+2]*q[2];
            /* A grazing cap has an infinite height slope. Bound that slope
             * before interpolation so malformed/extreme inputs stay finite. */
            const float scale=fmaxf(fabsf(n[0]),fmaxf(fabsf(n[1]),fabsf(n[2])));
            if(!(scale>0))continue;
            for(unsigned k=0;k<3;k++)n[k]/=scale;
            float nv=n[0]*view[0]+n[1]*view[1]+n[2]*view[2];
            if(nv<0){nv=-nv;for(unsigned k=0;k<3;k++)n[k]=-n[k];}
            nv=fmaxf(nv,1e-4f);
            for(unsigned k=0;k<3;k++)n[k]/=nv;
            /* A cap enters with finite height weight but unbounded slope.
             * Vanishing support removes its rim from interior shading without
             * changing coverage or depth. Hard/single surfaces retain normals. */
            if(support!=1)for(unsigned k=0;k<3;k++)n[k]*=support;
            const float d=l->depth[0]*q[0]+l->depth[1]*q[1]+l->depth[2]*q[2]+l->depth[3];
            coverage=fmaxf(coverage,fminf(1,residual/s->edge));
            if(!found){depth=d;memcpy(normal,n,sizeof normal);chord=z*support;mass=support;found=true;continue;}
            const float blend=h->controls[0];
            const float weight=blend>0?fmaxf(0,fminf(1,0.5f+0.5f*(d-depth)/blend)):(d>depth?1:0);
            depth=depth*(1-weight)+d*weight+blend*weight*(1-weight);
            for(unsigned k=0;k<3;k++)normal[k]=normal[k]*(1-weight)+n[k]*weight;
            chord=chord*(1-weight)+z*support*weight;
            mass=mass*(1-weight)+support*weight;
        }
        if(!found||!ell_unit(normal))continue;
        chord/=mass;
        const float dot=normal[0]*light[0]+normal[1]*light[1]+normal[2]*light[2],front=fmaxf(0,dot),back=fmaxf(0,-dot),
            nh=has_half?fmaxf(0,normal[0]*half[0]+normal[1]*half[1]+normal[2]*half[2]):0,
            spec=front>0&&nh>0&&m[2]>0?powf(nh,m[3]):0,thin=1-h->surface[2]*(1.0f/255)*chord;
        const uint16_t base=h->surface[0];unsigned color[3];
        float channels[3]={(base>>11)&31,(base>>5)&63,base&31};
        const unsigned max[3]={31,63,31};
        if(region&&base!=region->color){
            const float *r=region->plane,distance=r[0]*x+r[1]*yy+r[2]*depth+r[3],
                mix=inverse_feather>0?fmaxf(0,fminf(1,.5f+distance*inverse_feather)):(distance>=0?1:0);
            const uint16_t other=region->color;
            const unsigned b[3]={(other>>11)&31,(other>>5)&63,other&31};
            for(unsigned k=0;k<3;k++){
                if(mix>=1)channels[k]=(float)b[k];
                else if(mix>0)channels[k]+=((float)b[k]-channels[k])*mix;
            }
        }
        for(unsigned k=0;k<3;k++){
            const float c=channels[k]*(1.0f/max[k])*(m[0]*m[8+k]+m[1]*front*h->light[6+k])+
                h->light[6+k]*(m[2]*spec+m[4]*thin*back*m[5+k]);
            color[k]=(unsigned)lroundf(fmaxf(0,fminf(1,c))*max[k]);
        }
        const unsigned alpha=(unsigned)lroundf(h->surface[1]*coverage),ia=255-alpha;
        uint16_t *dest=pixels+(yy-y)*KSN_PROC_W+x;
        color[0]=(color[0]*alpha+((*dest>>11)&31)*ia+127)/255;
        color[1]=(color[1]*alpha+((*dest>>5)&63)*ia+127)/255;
        color[2]=(color[2]*alpha+(*dest&31)*ia+127)/255;
        *dest=(uint16_t)((color[0]<<11)|(color[1]<<5)|color[2]);
    }
}
/* A plain entry of a frame without pattern lines that reaches rows
 * y..y+height-1: the walk of ksn_proc_render_band. */
static inline __attribute__((always_inline)) void plain_entry(const ksn_proc_segment *s,uint16_t *pixels,
                                                              int y,int height){
    if(g_ksn_proc_hspan&&s->y0==s->y1){hspan(s,s->x0,pixels,y);return;}
    int x=s->x0,yy=s->y0,dx=abs(s->x1-s->x0),dy=abs(s->y1-s->y0);
    int sx=x<s->x1?1:-1,sy=yy<s->y1?1:-1,err=dx-dy;
    /* A band can be reached without replaying the line's prefix: the
     * Bresenham phase is rebuilt from its major-axis step count (clipping
     * the endpoints and restarting would change ties). band_entry does it in
     * 32 bits; the 64-bit form this replaced was a __divdi3 call per line and
     * piece, twice as many with 4-row pieces (lcd-dma-driver.md 9.9). */
    band_entry(s,s->x0,y,height,&x,&yy,&err);
    for(;;){
        if((sy>0&&yy>=y+height)||(sy<0&&yy<y))break;
        if(x>=0&&x<KSN_PROC_W&&yy>=y&&yy<y+height)pixels[(yy-y)*KSN_PROC_W+x]=s->color;
        if(x==s->x1&&yy==s->y1)break;
        int twice=2*err;
        if(twice>-dy){err-=dy;x+=sx;}
        if(twice<dx){err+=dx;yy+=sy;}
    }
}
/* Entry j, known to reach the rows: a pattern line takes j and j+1 (a
 * parameter entry's negative rows never pass a range test, so j is never one).
 * Out of line in the ext walk, so a frame without pattern lines runs its own
 * loop below as it always did. */
static __attribute__((noinline)) void ext_entry(const ksn_proc_frame *frame,unsigned j,
                                                            uint16_t *pixels,int y,int height){
    const ksn_proc_segment *s=&frame->segments[j];
        if(s->x0>=KSN_PROC_EXT_GOURAUD_PATCH-480){
            unsigned used;
            if(!gouraud_render_entry(s+1,frame->count-j-1,pixels,y,height,&used))return;
            return;
        }
        if(s->x0>=KSN_PROC_EXT_BLEND_REGION-480){
            ksn_proc_blend_ellipsoids p;ksn_proc_blend_region_mask mask;unsigned used;
            if(!ksn_proc_blend_region_read(s+1,frame->count-j-1,&p,&mask,&used))return;
            int l,t,r,b;if(!ksn_proc_blend_region_bounds(&p,&mask,&l,&t,&r,&b))return;
            if(s->y1>=y&&s->y0<y+height)blend_ellipsoids_band(&p,&mask,pixels,y,height);
            return;
        }
        if(s->x0>=KSN_PROC_EXT_CLIPPED_ELLIPSOID-480){
            ksn_proc_clipped_ellipsoid p;int winding;
            if(!ksn_proc_clipped_ellipsoid_read(s+1,frame->count-j-1,&p)||!clip_prepare(&p.clip,&winding))return;
            if(s->y1>=y&&s->y0<y+height)ellipsoid_band(&p.patch,&p.clip,winding,pixels,y,height);
            return;
        }
        if(s->x0>=KSN_PROC_EXT_BLEND_ELLIPSOIDS-480){
            ksn_proc_blend_ellipsoids p;unsigned used;
            if(!ksn_proc_blend_read(s+1,frame->count-j-1,&p,&used))return;
            if(s->y1>=y&&s->y0<y+height)blend_ellipsoids_band(&p,NULL,pixels,y,height);
            return;
        }
        if(s->x0>=KSN_PROC_EXT_ELLIPSOID-480){
            if(KSN_PROC_ELLIPSOID_WORDS>frame->count-j-1)return;
            ksn_proc_ellipsoid p;memcpy(&p,s+1,sizeof p);
            ellipsoid_band(&p,NULL,0,pixels,y,height);
            return;
        }
    if(s->x0>=KSN_PROC_EXT_MIN){
        if(j+1<frame->count)pattern_band(s,s+1,pixels,y,height);
    }else plain_band(s,pixels,y,height);
}
/* Packet validation owns typed scratch; plain Shiki frames must not reserve
 * it in the common range/carry walk's stack frame. */
static __attribute__((noinline)) bool packet_next(const ksn_proc_frame *frame,unsigned *j,
                                                 ksn_proc_segment *geometry){
    bool pattern;unsigned cost;
    return ksn_proc_frame_next(frame,j,geometry,&pattern,&cost);
}
/* The rest of an 8-row band after a first piece y..y+height-1 starting on the
 * band: what the carry notes, and what the next piece must ask for to use it. */
static inline bool carry_rest(int y,int height,int *rest_y,int *rest_end){
    if((y&7)||height>=8)return false;
    *rest_y=y+height;*rest_end=(y|7)+1;
    if(*rest_end>KSN_PROC_H)*rest_end=KSN_PROC_H;
    return *rest_y<*rest_end;
}
/* The walk over [first, end). With a carry: a first piece notes which entries
 * also reach the rest of its band, and the rest, asked for next with the same
 * frame and range, visits only those -- in the same order, so the pixels are
 * the same (docs/platform/lcd-dma-driver.md 9.9). Every other call leaves the
 * carry unused and invalid. */
static bool render_band(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height,
                        unsigned first,unsigned end,ksn_proc_carry *carry){
    if(!frame||!frame->ready||!pixels||y<0||height<0||y>KSN_PROC_H||height>KSN_PROC_H-y||
       frame->count>KSN_PROC_SEGMENTS||first>end||end>frame->count){if(carry)carry->valid=false;return false;}
    const bool ext=frame->ext;
    if(carry&&carry->valid&&carry->frame==frame&&carry->count==frame->count&&
       carry->y==y&&carry->height==height&&carry->first==first&&carry->end==end){
        carry->valid=false;
        for(unsigned w=first>>5;w<=(end?(end-1)>>5:0)&&first<end;w++){
            uint32_t bits=carry->bits[w];
            while(bits){
                const unsigned j=w*32u+(unsigned)__builtin_ctz(bits);
                bits&=bits-1u;
                if(ext)ext_entry(frame,j,pixels,y,height);
                else plain_entry(&frame->segments[j],pixels,y,height);
            }
        }
        return true;
    }
    int rest_y=0,rest_end=0;
    const bool note=carry&&carry_rest(y,height,&rest_y,&rest_end);
    if(carry){
        carry->valid=false;
        if(note)memset(carry->bits,0,sizeof carry->bits);
    }
    /* A first piece tests each entry against the whole band, as an 8-row
     * strip did: most of [first, end) is outside it and costs no more than
     * that. Only an entry inside is sorted into this piece, the rest, or
     * both. rest_y is y + height, so "reaches the rest" is ymax >= rest_y. */
    const int top=note?rest_end:y+height;
    if(ext){
        /* Advance by logical entries even when culled: payload bytes can look
         * like markers or visible line coordinates. Carry bits name headers. */
        for(unsigned j=first;j<end;){
            const unsigned start=j;
            ksn_proc_segment geometry;
            if(!packet_next(frame,&j,&geometry)||j>end)return false;
            const int ymin=geometry.y0<geometry.y1?geometry.y0:geometry.y1;
            const int ymax=geometry.y0>geometry.y1?geometry.y0:geometry.y1;
            if(ymax<y||ymin>=top)continue;
            if(note){
                if(ymax>=rest_y)carry->bits[start>>5]|=1u<<(start&31u);
                if(ymin>=rest_y)continue;
            }
            ext_entry(frame,start,pixels,y,height);
        }
    }else if(note)for(unsigned j=first;j<end;j++){
        const ksn_proc_segment *s=&frame->segments[j];
        int ymin=s->y0<s->y1?s->y0:s->y1;
        int ymax=s->y0>s->y1?s->y0:s->y1;
        if(ymax<y||ymin>=top)continue;
        if(ymax>=rest_y)carry->bits[j>>5]|=1u<<(j&31u);
        if(ymin>=rest_y)continue;
        plain_entry(s,pixels,y,height);
    }else for(unsigned j=first;j<end;j++){
        const ksn_proc_segment *s=&frame->segments[j];
        int ymin=s->y0<s->y1?s->y0:s->y1;
        int ymax=s->y0>s->y1?s->y0:s->y1;
        if(ymax<y||ymin>=y+height)continue;
        plain_entry(s,pixels,y,height);
    }
    if(note){
        carry->frame=frame;carry->count=frame->count;
        carry->y=(uint16_t)rest_y;carry->height=(uint16_t)(rest_end-rest_y);
        carry->first=(uint16_t)first;carry->end=(uint16_t)end;carry->valid=true;
    }
    return true;
}
bool ksn_proc_render_band(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height){
    return render_band(frame,pixels,y,height,0,frame?frame->count:0,NULL);
}
bool ksn_proc_render_band_range(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height,
                                unsigned first,unsigned end){
    return render_band(frame,pixels,y,height,first,end,NULL);
}
bool ksn_proc_render_band_carry(const ksn_proc_frame *frame,uint16_t *pixels,int y,int height,
                                unsigned first,unsigned end,ksn_proc_carry *carry){
    return render_band(frame,pixels,y,height,first,end,carry);
}
