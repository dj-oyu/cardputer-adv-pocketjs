#include "ksn_mesh_depth.h"
#include <math.h>
#include <string.h>
#include <limits.h>
bool ksn_mesh_append(ksn_mesh_snapshot *s,const ksn_proc_gouraud_patch *p,const float *depth,uint16_t node){
    int l,t,r,b;unsigned cost;
    if(!s||!s->enabled||!p||!depth||p->header.holes||
       !ksn_proc_gouraud_bounds(p,&l,&t,&r,&b,&cost)||
       s->work>s->work_limit||cost>s->work_limit-s->work)return false;
    co_vertex vertices[CO_MAX_VERTICES];
    for(unsigned j=0;j<p->header.vertices;j++){
        const ksn_proc_gouraud_vertex *v=p->vertices+j;
        vertices[j]=(co_vertex){v->x,v->y,v->mask,depth[j],v->color_a,v->color_b};
    }
    co_input in={vertices,p->indices,node,p->header.vertices,p->header.triangles};
    if(!co_append(&s->packets,&in))return false;
    s->work+=cost;return true;
}
typedef struct {double a,b,c;int sign;bool closed;} gouraud_edge;
static gouraud_edge gouraud_make_edge(const co_vertex *a,const co_vertex *b){
    const double dx=(double)b->x-a->x,dy=(double)b->y-a->y;
    gouraud_edge edge={.sign=1,.closed=dy<0||(dy==0&&dx>0)};
    /* Both triangles evaluate a shared edge in the same canonical direction.
     * Reversing it only negates the result, including exact zero at ties. */
    if(a->x>b->x||(a->x==b->x&&a->y>b->y)){
        const co_vertex *swap=a;a=b;b=swap;edge.sign=-1;
    }
    edge.a=(double)a->y-b->y;edge.b=(double)b->x-a->x;
    edge.c=((double)b->y-a->y)*a->x-((double)b->x-a->x)*a->y;return edge;
}
static double gouraud_edge_value(const gouraud_edge *e,int x,int y){return e->sign*(e->a*x+e->b*y+e->c);}

static bool strip(const ksn_mesh_snapshot *s,uint16_t *pixels,unsigned y,unsigned height,float *z){
    for(unsigned j=0;j<height*KSN_PROC_W;j++)z[j]=-INFINITY;
    size_t offset=0;
    while(offset<s->packets.size){
        co_view view;if(!co_read(s->packets.data+offset,s->packets.size-offset,&view))return false;
        for(unsigned face=0;face<view.triangle_count;face++){
            uint8_t indices[3];co_vertex v[3];
            if(!co_triangle_at(&view,face,indices))return false;
            for(unsigned j=0;j<3;j++)if(!co_vertex_at(&view,indices[j],v+j))return false;
            double area=((double)v[1].x-v[0].x)*((double)v[2].y-v[0].y)-((double)v[1].y-v[0].y)*((double)v[2].x-v[0].x);
            if(!(area>1e-5))return false;
            int left=(int)ceil(fmin(v[0].x,fmin(v[1].x,v[2].x))),right=(int)floor(fmax(v[0].x,fmax(v[1].x,v[2].x)));
            int top=(int)ceil(fmin(v[0].y,fmin(v[1].y,v[2].y))),bottom=(int)floor(fmax(v[0].y,fmax(v[1].y,v[2].y)));
            if(left<0)left=0;
            if(right>=KSN_PROC_W)right=KSN_PROC_W-1;
            if(top<(int)y)top=(int)y;
            if(bottom>=(int)(y+height))bottom=(int)(y+height)-1;
            const gouraud_edge edge[3]={gouraud_make_edge(v+1,v+2),gouraud_make_edge(v+2,v),gouraud_make_edge(v,v+1)};
            for(int yy=top;yy<=bottom;yy++)for(int x=left;x<=right;x++){
                double w[3];bool inside=true;
                for(unsigned k=0;k<3;k++){w[k]=gouraud_edge_value(edge+k,x,yy);if(w[k]<0||(w[k]==0&&!edge[k].closed))inside=false;}
                if(!inside)continue;
                for(unsigned k=0;k<3;k++)w[k]/=area;
                const float depth=(float)(w[0]*v[0].depth+w[1]*v[1].depth+w[2]*v[2].depth);
                if(!isfinite(depth))return false;
                const unsigned at=(unsigned)(yy-(int)y)*KSN_PROC_W+(unsigned)x;
                if(depth<=z[at])continue;
                z[at]=depth;
                const bool second=w[0]*v[0].mask+w[1]*v[1].mask+w[2]*v[2].mask>=0;
                const uint16_t a=second?v[0].color_b:v[0].color_a,b=second?v[1].color_b:v[1].color_a,c=second?v[2].color_b:v[2].color_a;
                const float cw[3]={(float)w[0],(float)w[1],(float)w[2]};
                const unsigned red=(unsigned)lroundf(fmaxf(0,fminf(31,cw[0]*(a>>11)+cw[1]*(b>>11)+cw[2]*(c>>11)))),
                    green=(unsigned)lroundf(fmaxf(0,fminf(63,cw[0]*((a>>5)&63)+cw[1]*((b>>5)&63)+cw[2]*((c>>5)&63)))),
                    blue=(unsigned)lroundf(fmaxf(0,fminf(31,cw[0]*(a&31)+cw[1]*(b&31)+cw[2]*(c&31))));
                pixels[at]=(uint16_t)((red<<11)|(green<<5)|blue);
            }
        }
        offset+=view.size;
    }
    return true;
}
bool ksn_mesh_render(const ksn_mesh_snapshot *s,uint16_t *pixels,unsigned y,unsigned height,float *scratch,unsigned rows){
    if(!s||!s->enabled||!pixels||!scratch||(rows!=1&&rows!=8)||y>KSN_PROC_H||height>KSN_PROC_H-y)return false;
    for(unsigned at=0;at<height;at+=rows){unsigned n=height-at;if(n>rows)n=rows;
        if(!strip(s,pixels+at*KSN_PROC_W,y+at,n,scratch))return false;}
    return true;
}
