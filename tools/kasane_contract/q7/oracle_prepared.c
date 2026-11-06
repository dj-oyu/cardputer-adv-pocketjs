#include "ui/kasane/ksn_mesh_prepared.h"
uint8_t *test_owner;
#include <math.h>
#include <string.h>
#ifdef KSN_MESH_EDGE4_HOST_STATS
uint64_t ksn_mesh_prepared_edge4_stats[4];
#endif
typedef ksn_prepared_edge gouraud_edge;
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


size_t ksn_mesh_prepared_bytes(const ksn_mesh_snapshot *s){
    if(!s||!s->enabled)return 0;
    size_t off=0;unsigned count=0;
    while(off<s->packets.size){
        co_view view;if(!co_read(s->packets.data+off,s->packets.size-off,&view))return 0;
        count+=view.triangle_count;if(count>KSN_PREPARED_MAX_TRIANGLES)return 0;
        off+=view.size;
    }
    return sizeof(ksn_mesh_prepared)+count*sizeof(ksn_prepared_triangle);
}
bool ksn_mesh_prepare(const ksn_mesh_snapshot *s,ksn_mesh_prepared *out,size_t bytes){
    if(!out||bytes<sizeof *out)return false;
    out->valid=false;out->count=0;memset(out->bands,0,sizeof out->bands);
    size_t cap=(bytes-sizeof *out)/sizeof out->triangles[0];
    out->capacity=(unsigned)(cap>KSN_PREPARED_MAX_TRIANGLES?KSN_PREPARED_MAX_TRIANGLES:cap);
    if(!s||!s->enabled)return false;
    size_t offset=0;
    while(offset<s->packets.size){
        co_view view;if(!co_read(s->packets.data+offset,s->packets.size-offset,&view))return false;
        for(unsigned face=0;face<view.triangle_count;face++){
            if(out->count==out->capacity)return false;
            ksn_prepared_triangle *t=&out->triangles[out->count];
            co_vertex *v=t->v;uint8_t indices[3];bool back;
            if(!co_triangle_at(&view,face,indices)||!co_triangle_side(&view,face,&back))return false;
            for(unsigned j=0;j<3;j++)if(!co_vertex_at_side(&view,indices[j],back,v+j))return false;
            double area=((double)v[1].x-v[0].x)*((double)v[2].y-v[0].y)-((double)v[1].y-v[0].y)*((double)v[2].x-v[0].x);
            if(!(area>1e-5))return false;
            t->inverse=1/area;
            t->left=(int)ceil(fmin(v[0].x,fmin(v[1].x,v[2].x)));
            t->right=(int)floor(fmax(v[0].x,fmax(v[1].x,v[2].x)));
            t->top=(int)ceil(fmin(v[0].y,fmin(v[1].y,v[2].y)));
            t->bottom=(int)floor(fmax(v[0].y,fmax(v[1].y,v[2].y)));
            if(t->left<0)t->left=0;
            if(t->right>=KSN_PROC_W)t->right=KSN_PROC_W-1;
            if(t->top<0)t->top=0;
            if(t->bottom>=KSN_PROC_H)t->bottom=KSN_PROC_H-1;
            t->edge[0]=gouraud_make_edge(v+1,v+2);t->edge[1]=gouraud_make_edge(v+2,v);t->edge[2]=gouraud_make_edge(v,v+1);
#ifdef KSN_MESH_PREPARED_EDGE4
            double abc[3][3];
            for(unsigned k=0;k<3;k++){
                abc[k][0]=t->edge[k].sign*t->edge[k].a;
                abc[k][1]=t->edge[k].sign*t->edge[k].b;
                abc[k][2]=t->edge[k].sign*t->edge[k].c;
            }
            t->classify_valid=pie_edge4_prepare(abc,&t->classify);
#endif
            if(t->left<=t->right&&t->top<=t->bottom)
                for(unsigned band=(unsigned)t->top/8;band<=(unsigned)t->bottom/8;band++)out->bands[band]|=UINT64_C(1)<<out->count;
            out->count++;
        }
        offset+=view.size;
    }
    out->valid=true;return true;
}
static bool prepared_strip(const ksn_mesh_prepared *s,uint16_t *pixels,unsigned y,unsigned height,float *z){
    for(unsigned j=0;j<height*KSN_PROC_W;j++)z[j]=-INFINITY;
    uint64_t pending=0;
    for(unsigned b=y/8;b<=(y+height-1)/8;b++)pending|=s->bands[b];
    while(pending){
        unsigned face=(unsigned)__builtin_ctzll(pending);pending&=pending-1;
        const ksn_prepared_triangle *t=&s->triangles[face];
        const co_vertex *v=t->v;const gouraud_edge *edge=t->edge;const double inverse=t->inverse;
        int left=t->left,right=t->right,top=t->top,bottom=t->bottom;
        if(top<(int)y)top=(int)y;
        if(bottom>=(int)(y+height))bottom=(int)(y+height)-1;
            for(int yy=top;yy<=bottom;yy++){
#ifdef KSN_MESH_PREPARED_EDGE4
                pie_edge4_lanes lanes;pie_edge4_mask rejects;
                bool classify=t->classify_valid&&left<=right&&pie_edge4_init(&t->classify,(unsigned)left,(unsigned)yy,&lanes);
#endif
                for(int base=left;base<=right;base+=4){
#ifdef KSN_MESH_PREPARED_EDGE4
                    if(classify){
                        pie_edge4_reject_raw(&lanes,&rejects);
#ifdef KSN_MESH_EDGE4_HOST_STATS
                        ++ksn_mesh_prepared_edge4_stats[0];
                        if(rejects.v[0]&&rejects.v[1]&&rejects.v[2]&&rejects.v[3])++ksn_mesh_prepared_edge4_stats[1];
#endif
                    }
#endif
                    for(int lane=0;lane<4&&base+lane<=right;lane++){
                        int x=base+lane;
#ifdef KSN_MESH_PREPARED_EDGE4
#ifdef KSN_MESH_EDGE4_HOST_STATS
                        ++ksn_mesh_prepared_edge4_stats[2];
#endif
                        if(classify&&rejects.v[lane]){
#ifdef KSN_MESH_EDGE4_HOST_STATS
                            ++ksn_mesh_prepared_edge4_stats[3];
#endif
                            continue;
                        }
#endif

                double w[3];bool inside=true;
                for(unsigned k=0;k<3;k++){w[k]=gouraud_edge_value(edge+k,x,yy);if(w[k]<0||(w[k]==0&&!edge[k].closed))inside=false;}
                if(!inside)continue;
                for(unsigned k=0;k<3;k++)w[k]*=inverse;
                const float depth=(float)(w[0]*v[0].depth+w[1]*v[1].depth+w[2]*v[2].depth);
                if(!isfinite(depth))return false;
                const unsigned at=(unsigned)(yy-(int)y)*KSN_PROC_W+(unsigned)x;
                if(depth<=z[at])continue;
                z[at]=depth; if(test_owner) test_owner[at]=(uint8_t)face;
                const bool second=w[0]*v[0].mask+w[1]*v[1].mask+w[2]*v[2].mask>=0;
                const uint16_t a=second?v[0].color_b:v[0].color_a,b=second?v[1].color_b:v[1].color_a,c=second?v[2].color_b:v[2].color_a;
                const float cw[3]={(float)w[0],(float)w[1],(float)w[2]};
                const unsigned red=(unsigned)lroundf(fmaxf(0,fminf(31,cw[0]*(a>>11)+cw[1]*(b>>11)+cw[2]*(c>>11)))),
                    green=(unsigned)lroundf(fmaxf(0,fminf(63,cw[0]*((a>>5)&63)+cw[1]*((b>>5)&63)+cw[2]*((c>>5)&63)))),
                    blue=(unsigned)lroundf(fmaxf(0,fminf(31,cw[0]*(a&31)+cw[1]*(b&31)+cw[2]*(c&31))));
                pixels[at]=(uint16_t)((red<<11)|(green<<5)|blue);
                    }
#ifdef KSN_MESH_PREPARED_EDGE4
                    if(classify&&base+4<=right)pie_edge4_advance(&t->classify,&lanes);
#endif
                }
            }

    }
    return true;
}
bool ksn_mesh_prepared_render(const ksn_mesh_prepared *s,uint16_t *pixels,unsigned y,unsigned height,float *scratch,unsigned rows){
    if(!s||!s->valid||!pixels||!scratch||(rows!=1&&rows!=8)||y>KSN_PROC_H||height>KSN_PROC_H-y)return false;
    for(unsigned at=0;at<height;at+=rows){unsigned n=height-at;if(n>rows)n=rows;
        if(!prepared_strip(s,pixels+at*KSN_PROC_W,y+at,n,scratch))return false;}
    return true;
}
