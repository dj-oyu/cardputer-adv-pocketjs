#include "ayatsuri_model.h"
#include "ayatsuri_transform.h"
#include <limits.h>

typedef struct {
    double opacity;
    uint32_t offset, color;
    uint8_t enabled, world_enabled;
} model_node_state;

static uint16_t m16(const uint8_t *p){return (uint16_t)((uint16_t)p[0]|(uint16_t)p[1]<<8);}
static uint32_t m32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static int parent_id(const uint8_t *p){const uint16_t x=m16(p);return x<=INT16_MAX?(int)x:(int)x-65536;}
static float mf(const uint8_t *p){uint32_t bits=m32(p);float f;memcpy(&f,&bits,4);return f;}
static double md(const uint8_t *p){uint64_t bits=(uint64_t)m32(p)|(uint64_t)m32(p+4)<<32;double d;memcpy(&d,&bits,8);return d;}
static bool unit(double n){return isfinite(n)&&n>=0&&n<=1;}
static void add_count(uint32_t *counter,uint32_t n){*counter=UINT32_MAX-*counter<n?UINT32_MAX:*counter+n;}
static bool overlaps(const void *a,size_t an,const void *b,size_t bn){
    const uintptr_t x=(uintptr_t)a,y=(uintptr_t)b;
    return an&&bn&&(x<=y?y-x<an:x-y<bn);
}
static const uint8_t *row(const ayatsuri_model *m,unsigned section,uint32_t index,unsigned stride){
    return m->data+m->section[section-1].offset+index*stride;
}
static unsigned slot_bytes(const uint8_t *node){return node[2]==0?84:node[2]==1?264:316+108u*m16(node+6);}
static bool float_rows_valid(const uint8_t *data,uint32_t count,unsigned floats,unsigned positive_start){
    for(uint32_t i=0;i<count;++i)for(unsigned k=0;k<floats;++k){
        const float value=mf(data+(i*floats+k)*4);
        if(!isfinite(value)||(k>=positive_start&&!(value>0)))return false;
    }
    return true;
}
ayatsuri_model_status ayatsuri_model_open(const void *data,size_t bytes,ayatsuri_model *out){
    const uint32_t endian=1;
    if(*(const uint8_t *)&endian!=1)return AYATSURI_MODEL_ENDIAN;
    if(!data||!out||bytes<32||bytes>AYATSURI_MODEL_MAX_BYTES||overlaps(out,sizeof *out,data,bytes))
        return AYATSURI_MODEL_INVALID;
    const uint8_t *p=data;const uint32_t sections=m32(p+12);
    if(memcmp(p,"AYNM",4)||m16(p+4)!=1||m16(p+6)!=32||m32(p+8)!=bytes||
       sections<7||sections>AYATSURI_MODEL_MAX_SECTIONS||m32(p+16)!=32||m32(p+20)!=20||m32(p+24)||m32(p+28)||
       sections>(bytes-32)/20)return AYATSURI_MODEL_INVALID;
    const uint32_t directory_end=32+20*sections;
    ayatsuri_model candidate={.data=p,.bytes=(uint32_t)bytes};unsigned seen=0;
    static const uint32_t strides[8]={16,36,12,24,72,8,1,32};
    for(uint32_t i=0;i<sections;++i){
        const uint8_t *d=p+32+20*i;const uint16_t type=m16(d),flags=m16(d+2);
        const uint32_t offset=m32(d+4),length=m32(d+8),count=m32(d+12),stride=m32(d+16);
        if(flags>1||(offset&7)||offset<directory_end||offset>bytes||length>bytes-offset)
            return AYATSURI_MODEL_INVALID;
        for(uint32_t j=0;j<i;++j){
            const uint8_t *prior=p+32+20*j;
            if(overlaps(p+offset,length,p+m32(prior+4),m32(prior+8)))return AYATSURI_MODEL_INVALID;
        }
        if(type>=1&&type<=8){
            const unsigned bit=1u<<(type-1);
            if(flags!=1||(seen&bit)||stride!=strides[type-1]||(uint64_t)count*stride!=length)
                return AYATSURI_MODEL_INVALID;
            candidate.section[type-1]=(ayatsuri_model_section){offset,count};seen|=bit;
        }else if(flags&1)return AYATSURI_MODEL_INVALID;
        else if(stride&&(uint64_t)count*stride>length)return AYATSURI_MODEL_INVALID;
    }
    if((seen&127)!=127)return AYATSURI_MODEL_INVALID;
    const uint32_t count=candidate.section[0].count;
    if(!count||count>AYATSURI_MODEL_MAX_NODES||candidate.section[1].count!=count||candidate.section[5].count!=count||
       candidate.section[2].count>count||candidate.section[3].count>8*count||candidate.section[4].count>count||
       candidate.section[6].count>127*count||
       ((seen&128)&&(!candidate.section[7].count||candidate.section[7].count>count)))return AYATSURI_MODEL_INVALID;
    candidate.node_count=(uint16_t)count;
    for(uint32_t k=0;k<candidate.section[6].count;++k){const uint8_t c=*row(&candidate,7,k,1);
        if(c<32||c>126)return AYATSURI_MODEL_INVALID;}
    if(!float_rows_valid(row(&candidate,2,0,36),count,9,9)||
       !float_rows_valid(row(&candidate,3,0,12),candidate.section[2].count,3,0)||
       !float_rows_valid(row(&candidate,4,0,24),candidate.section[3].count,6,3))return AYATSURI_MODEL_INVALID;
    for(uint32_t i=0;i<candidate.section[4].count;++i){
        const uint8_t *material=row(&candidate,5,i,72);
        for(unsigned k=0;k<11;++k){
            const float value=mf(material+4*k);
            if(k==3){if(!isfinite(value)||value<1||value>128)return AYATSURI_MODEL_INVALID;}
            else if(!unit(value))return AYATSURI_MODEL_INVALID;
        }
        if(m32(material+44)>0xffffff||!unit(md(material+48))||!unit(md(material+56))||!unit(md(material+64)))
            return AYATSURI_MODEL_INVALID;
    }
    for(uint32_t i=0;i<count;++i){
        const uint8_t *n=row(&candidate,1,i,16),*pose=row(&candidate,2,i,36);
        const int parent=parent_id(n);const uint16_t shape=m16(n+4),lobes=m16(n+6),material=m16(n+8),binding=m16(n+10);
        const float blend=mf(n+12);
        if(parent< -1||parent>=(int)i||n[2]>2||n[3]>1||binding>=count||!isfinite(blend))return AYATSURI_MODEL_INVALID;
        for(unsigned k=6;k<9;++k)if(mf(pose+4*k)==0)return AYATSURI_MODEL_INVALID;
        if(!n[2]){if(shape!=UINT16_MAX||material!=UINT16_MAX||lobes||blend!=0)return AYATSURI_MODEL_INVALID;}
        else{
            if(material>=candidate.section[4].count)return AYATSURI_MODEL_INVALID;
            if(n[2]==1){if(shape>=candidate.section[2].count||lobes||blend!=0)return AYATSURI_MODEL_INVALID;}
            else if(!lobes||lobes>8||shape>candidate.section[3].count||lobes>candidate.section[3].count-shape||blend<0||blend>128)
                return AYATSURI_MODEL_INVALID;
            ++candidate.packet_count;
        }
        const uint8_t *b=row(&candidate,6,binding,8);const uint16_t length=m16(b+2);const uint32_t start=m32(b+4);
        if(m16(b)!=i||!length||length>127||start>candidate.section[6].count||length>candidate.section[6].count-start)
            return AYATSURI_MODEL_INVALID;
        const uint8_t *name=row(&candidate,7,start,1);
        for(unsigned k=0;k<length;++k)if(name[k]<32||name[k]>126)return AYATSURI_MODEL_INVALID;
        for(uint32_t j=0;j<i;++j){
            const uint8_t *prior_node=row(&candidate,1,j,16),*prior=row(&candidate,6,m16(prior_node+10),8);
            if(m16(prior+2)==length&&!memcmp(name,row(&candidate,7,m32(prior+4),1),length))return AYATSURI_MODEL_INVALID;
        }
        candidate.slot_bytes+=slot_bytes(n);
    }
    /* Validate all cold region rows before publishing any model metadata.
     * A normal is already unit length on the wire; never silently normalize. */
    for(uint32_t k=0;k<candidate.section[7].count;++k){
        const uint8_t *region=row(&candidate,8,k,32);const uint16_t id=m16(region);
        const double x=mf(region+4),y=mf(region+8),z=mf(region+12);
        const float offset=mf(region+16),feather=mf(region+20);
        if(id>=count||row(&candidate,1,id,16)[2]!=1||region[2]!=4||region[3]||m32(region+28)||
           !isfinite(x)||!isfinite(y)||!isfinite(z)||fabs(x*x+y*y+z*z-1)>1e-4||
           !isfinite(offset)||offset< -2||offset>2||!isfinite(feather)||
           (feather!=0&&(feather<1e-4f||feather>2))||m32(region+24)>0xffffff)return AYATSURI_MODEL_INVALID;
        for(uint32_t j=0;j<k;++j)if(m16(row(&candidate,8,j,32))==id)return AYATSURI_MODEL_INVALID;
    }
    *out=candidate;return AYATSURI_MODEL_OK;
}
ayatsuri_model_status ayatsuri_model_find_node(const ayatsuri_model *m,const char *name,size_t bytes,uint16_t *out){
    if(!m||!m->data||!name||!out||!bytes||bytes>127||overlaps(out,sizeof *out,m,sizeof *m)||
       overlaps(out,sizeof *out,m->data,m->bytes))return AYATSURI_MODEL_INVALID;
    for(unsigned i=0;i<m->node_count;++i){const uint8_t *b=row(m,6,i,8);
        if(m16(b+2)==bytes&&!memcmp(row(m,7,m32(b+4),1),name,bytes)){*out=m16(b);return AYATSURI_MODEL_OK;}}
    return AYATSURI_MODEL_DONE;
}
size_t ayatsuri_model_instance_bytes(const ayatsuri_model *m){
    return m&&m->data?((sizeof(model_node_state)*m->node_count+7u)&~(size_t)7u)+m->slot_bytes:0;
}
static model_node_state *states(ayatsuri_model_instance *i){return i->arena;}
static float *work(ayatsuri_model_instance *i,unsigned node){return (float *)((uint8_t *)i->arena+states(i)[node].offset);}
static ayatsuri_model_status enter(ayatsuri_model_instance *i,const void *owner,uint32_t generation){
    if(!i||!i->alive||!owner||i->owner!=owner||i->generation!=generation)return AYATSURI_MODEL_STATE;
    if(i->busy)return AYATSURI_MODEL_BUSY;
    i->busy=true;return AYATSURI_MODEL_OK;
}
static ayatsuri_model_status leave(ayatsuri_model_instance *i,ayatsuri_model_status status){i->busy=false;return status;}
ayatsuri_model_status ayatsuri_model_instance_init(const ayatsuri_model *m,void *arena,size_t bytes,
        const void *owner,uint32_t generation,ayatsuri_model_instance *out){
    const size_t need=ayatsuri_model_instance_bytes(m);
    if(!need||!arena||!owner||!out||!generation||((uintptr_t)arena&7)||bytes<need||
       overlaps(arena,need,m->data,m->bytes)||overlaps(arena,need,m,sizeof *m)||
       overlaps(arena,need,out,sizeof *out)||overlaps(out,sizeof *out,m,sizeof *m)||
       overlaps(out,sizeof *out,m->data,m->bytes))return AYATSURI_MODEL_INVALID;
    memset(arena,0,need);
    ayatsuri_model_instance result={.model=m,.arena=arena,.owner=owner,.arena_bytes=need,.generation=generation,.alive=true};
    uint32_t offset=(uint32_t)((sizeof(model_node_state)*m->node_count+7u)&~(size_t)7u);
    for(unsigned j=0;j<m->node_count;++j){
        const uint8_t *node=row(m,1,j,16);model_node_state *s=states(&result)+j;
        s->offset=offset;s->enabled=node[3];memcpy(work(&result,j)+12,row(m,2,j,36),36);
        add_count(&result.stats.init_copy_bytes,36);
        if(node[2]){
            const uint8_t *material=row(m,5,m16(node+8),72);
            s->color=m32(material+44);s->opacity=md(material+56);
            memcpy((uint8_t *)work(&result,j)+(node[2]==1?164:264),material,node[2]==1?32:44);
            add_count(&result.stats.init_copy_bytes,node[2]==1?32:44);
            if(node[2]==1){work(&result,j)[63]=work(&result,j)[64]=1;}
            else work(&result,j)[77]=mf(node+12);
        }
        offset+=slot_bytes(node);
    }
    *out=result;return AYATSURI_MODEL_OK;
}
ayatsuri_model_status ayatsuri_model_instance_dispose(ayatsuri_model_instance *i,const void *owner,uint32_t generation){
    if(!i||!owner||i->owner!=owner||i->generation!=generation)return AYATSURI_MODEL_STATE;
    if(i->busy)return AYATSURI_MODEL_BUSY;
    i->alive=false;i->evaluated=false;i->arena=NULL;i->arena_bytes=0;return AYATSURI_MODEL_OK;
}
ayatsuri_model_status ayatsuri_model_instance_targets(ayatsuri_model_instance *i,const void *owner,
        uint32_t generation,const ayatsuri_model_targets *t){
    ayatsuri_model_status status=enter(i,owner,generation);if(status)return status;
    const size_t count=i->model->node_count;
    if(!t||t->count!=count||!t->dirty||!t->pose||!t->color||!t->opacity||
       overlaps(t->dirty,count,i->arena,i->arena_bytes)||overlaps(t->pose,count*36,i->arena,i->arena_bytes)||
       overlaps(t->color,count*4,i->arena,i->arena_bytes)||overlaps(t->opacity,count*8,i->arena,i->arena_bytes))
        return leave(i,AYATSURI_MODEL_INVALID);
    for(unsigned j=0;j<count;++j){
        const unsigned flags=t->dirty[j];const uint8_t *node=row(i->model,1,j,16);
        if(flags>127||(flags&24)==24||(!node[2]&&(flags&96)))return leave(i,AYATSURI_MODEL_INVALID);
        for(unsigned k=0;k<9;++k)if(flags&(1u<<(k/3))){const float v=t->pose[j*9+k];
            if(!isfinite(v)||(k>=6&&v==0))return leave(i,AYATSURI_MODEL_INVALID);}
        if((flags&32)&&t->color[j]>0xffffff)return leave(i,AYATSURI_MODEL_INVALID);
        if((flags&64)&&!unit(md((const uint8_t *)t->opacity+8*j)))return leave(i,AYATSURI_MODEL_INVALID);
    }
    bool changed=false;
    for(unsigned j=0;j<count;++j){
        const unsigned flags=t->dirty[j];model_node_state *s=states(i)+j;float *pose=work(i,j)+12;
        bool node_changed=false;
        for(unsigned k=0;k<9;++k)if((flags&(1u<<(k/3)))&&pose[k]!=t->pose[j*9+k]){
            pose[k]=t->pose[j*9+k];node_changed=true;add_count(&i->stats.target_copy_bytes,4);}
        if((flags&24)&&s->enabled!=!!(flags&8)){s->enabled=!!(flags&8);node_changed=true;add_count(&i->stats.target_copy_bytes,1);}
        if((flags&32)&&s->color!=t->color[j]){s->color=t->color[j];node_changed=true;add_count(&i->stats.target_copy_bytes,4);}
        if(flags&64){const double value=md((const uint8_t *)t->opacity+8*j);
            if(s->opacity!=value){s->opacity=value;node_changed=true;add_count(&i->stats.target_copy_bytes,8);}}
        if(node_changed){changed=true;add_count(&i->stats.target_changed,1);}
    }
    if(changed){add_count(&i->mutation_epoch,1);i->evaluated=false;}
    return leave(i,AYATSURI_MODEL_OK);
}
ayatsuri_model_status ayatsuri_model_instance_read_pose(ayatsuri_model_instance *i,const void *owner,
        uint32_t generation,uint16_t node,float out[9]){
    ayatsuri_model_status status=enter(i,owner,generation);if(status)return status;
    if(!out||node>=i->model->node_count||overlaps(out,36,i,sizeof *i)||
       overlaps(out,36,i->model,sizeof *i->model)||overlaps(out,36,i->arena,i->arena_bytes)||
       overlaps(out,36,i->model->data,i->model->bytes))return leave(i,AYATSURI_MODEL_INVALID);
    memcpy(out,work(i,node)+12,36);return leave(i,AYATSURI_MODEL_OK);
}
ayatsuri_model_status ayatsuri_model_instance_stats(ayatsuri_model_instance *i,const void *owner,
        uint32_t generation,ayatsuri_model_stats *out){
    ayatsuri_model_status status=enter(i,owner,generation);if(status)return status;
    if(!out||overlaps(out,sizeof *out,i,sizeof *i)||overlaps(out,sizeof *out,i->model,sizeof *i->model)||
       overlaps(out,sizeof *out,i->arena,i->arena_bytes)||
       overlaps(out,sizeof *out,i->model->data,i->model->bytes))return leave(i,AYATSURI_MODEL_INVALID);
    *out=i->stats;return leave(i,AYATSURI_MODEL_OK);
}

/* QuickJS Math.hypot folds the platform hypot left-to-right. The camera's
 * double basis is narrowed only at the same typed-array stores as the JS. */
static double hypot3(double x,double y,double z){return hypot(hypot(x,y),z);}
static bool narrow(double v,float *out){
    const double magnitude=fabs(v),midpoint=(double)FLT_MAX+0x1p103;
    if(!isfinite(v)||magnitude>=midpoint)return false;
    *out=magnitude>(double)FLT_MAX?(v<0?-FLT_MAX:FLT_MAX):(float)v;return true;
}
static uint16_t pack_rgb(uint32_t color){
    return (uint16_t)(floor((color>>16)*31.0/255+0.5)*2048+
        floor(((color>>8)&255)*63.0/255+0.5)*32+floor((color&255)*31.0/255+0.5));
}
static void surface_write(float *storage,unsigned offset,unsigned index,uint16_t value){
    memcpy((uint8_t *)storage+offset+2*index,&value,2);
}
static bool ellipse_packet(ayatsuri_model_instance *i,unsigned id,const float right[3],const float down[3]){
    const ayatsuri_model *model=i->model;const uint8_t *node=row(model,1,id,16),*r=row(model,3,m16(node+4),12);
    const uint8_t *material=row(model,5,m16(node+8),72);float *w=work(i,id);
    double m[12];for(unsigned k=0;k<12;++k)m[k]=w[k];
    const double radii[3]={mf(r),mf(r+4),mf(r+8)},
        a=m[0]*radii[0],b=m[1]*radii[1],c=m[2]*radii[2],d=m[4]*radii[0],e=m[5]*radii[1],f=m[6]*radii[2],
        g=m[8]*radii[0],h=m[9]*radii[1],j=m[10]*radii[2],det=a*(e*j-f*h)-b*(d*j-f*g)+c*(d*h-e*g);
    if(!isfinite(det)||fabs(det)<1e-12)return false;
    const double normal[9]={(e*j-f*h)/det,(f*g-d*j)/det,(d*h-e*g)/det,(c*h-b*j)/det,(a*j-c*g)/det,
        (b*g-a*h)/det,(b*f-c*e)/det,(c*d-a*f)/det,(a*e-b*d)/det};
    for(unsigned k=0;k<9;++k)if(!narrow(normal[k],w+32+k))return false;
    const double zoom=i->camera[6];
    for(unsigned k=0;k<3;++k){
        if(!narrow(zoom*(right[0]*m[k]+right[1]*m[4+k]+right[2]*m[8+k])*radii[k],w+24+k)||
           !narrow(zoom*(down[0]*m[k]+down[1]*m[4+k]+down[2]*m[8+k])*radii[k],w+28+k))return false;
    }
    const double x=m[3]-i->camera[3],y=m[7]-i->camera[4],z=m[11]-i->camera[5];
    if(!narrow(i->camera[7]+zoom*(right[0]*x+right[1]*y+right[2]*z),w+27)||
       !narrow(i->camera[8]+zoom*(down[0]*x+down[1]*y+down[2]*z),w+31))return false;
    const uint16_t color=pack_rgb(states(i)[id].color),opacity=(uint16_t)floor(states(i)[id].opacity*255+0.5),
        thickness=(uint16_t)floor(fmin(1,md(material+48)*hypot3(m[2],m[6],m[10]))*255+0.5);
    surface_write(w,232,0,color);surface_write(w,232,1,color);surface_write(w,232,2,opacity);
    surface_write(w,232,3,thickness);surface_write(w,232,4,0);
    /* No UV overrides. Match default JS arithmetic,
     * including the negative zero produced by -sin(0). */
    w[49]=1;w[50]=-0.0f;w[51]=0;w[52]=0;w[53]=1;w[54]=0;
    if(!narrow(md(material+64),w+55))return false;
    w[56]=0;w[57]=1;
    for(uint32_t k=0;k<model->section[7].count;++k){
        const uint8_t *region=row(model,8,k,32);
        if(m16(region)!=id)continue;
        surface_write(w,232,1,pack_rgb(m32(region+24)));surface_write(w,232,4,4);
        for(unsigned j=0;j<5;++j)w[49+j]=mf(region+4+4*j);
        w[54]=0;break;
    }
    return true;
}
static bool blend_packet(ayatsuri_model_instance *i,unsigned id,const float right[3],const float down[3],const float forward[3]){
    const ayatsuri_model *model=i->model;const uint8_t *node=row(model,1,id,16),*material=row(model,5,m16(node+8),72);
    const unsigned count=m16(node+6);float *w=work(i,id),*packet=w+79+6*count;
    double m[12];for(unsigned k=0;k<12;++k)m[k]=w[k];
    const double zoom=i->camera[6];
    for(unsigned l=0;l<count;++l){
        const uint8_t *local=row(model,4,m16(node+4)+l,24);
        const double x=mf(local),y=mf(local+4),z=mf(local+8),rx=mf(local+12),ry=mf(local+16),rz=mf(local+20),
            a=m[0]*rx,b=m[1]*ry,c=m[2]*rz,d=m[4]*rx,e=m[5]*ry,f=m[6]*rz,g=m[8]*rx,h=m[9]*ry,j=m[10]*rz,
            wx=m[0]*x+m[1]*y+m[2]*z+m[3]-i->camera[3],wy=m[4]*x+m[5]*y+m[6]*z+m[7]-i->camera[4],
            wz=m[8]*x+m[9]*y+m[10]*z+m[11]-i->camera[5],det=a*(e*j-f*h)-b*(d*j-f*g)+c*(d*h-e*g);
        if(!isfinite(det)||fabs(det)<1e-12)return false;
        const double values[21]={
            zoom*(right[0]*a+right[1]*d+right[2]*g),zoom*(right[0]*b+right[1]*e+right[2]*h),
            zoom*(right[0]*c+right[1]*f+right[2]*j),i->camera[7]+zoom*(right[0]*wx+right[1]*wy+right[2]*wz),
            zoom*(down[0]*a+down[1]*d+down[2]*g),zoom*(down[0]*b+down[1]*e+down[2]*h),
            zoom*(down[0]*c+down[1]*f+down[2]*j),i->camera[8]+zoom*(down[0]*wx+down[1]*wy+down[2]*wz),
            (e*j-f*h)/det,(f*g-d*j)/det,(d*h-e*g)/det,(c*h-b*j)/det,(a*j-c*g)/det,(b*g-a*h)/det,
            (b*f-c*e)/det,(c*d-a*f)/det,(a*e-b*d)/det,
            forward[0]*a+forward[1]*d+forward[2]*g,forward[0]*b+forward[1]*e+forward[2]*h,
            forward[0]*c+forward[1]*f+forward[2]*j,forward[0]*wx+forward[1]*wy+forward[2]*wz};
        for(unsigned k=0;k<21;++k){
            if(!narrow(values[k],packet+21*l+k)||fabs(packet[21*l+k])>(k>=8&&k<17?1e6:4096))return false;
        }
    }
    surface_write(w,252,0,pack_rgb(states(i)[id].color));
    surface_write(w,252,1,(uint16_t)floor(states(i)[id].opacity*255+0.5));
    surface_write(w,252,2,(uint16_t)floor(fmin(1,md(material+48)*hypot3(m[2],m[6],m[10]))*255+0.5));
    return narrow(md(material+64),w+78);
}
ayatsuri_model_status ayatsuri_model_instance_evaluate(ayatsuri_model_instance *i,const void *owner,
        uint32_t generation,const float camera[9],const float light[6]){
    ayatsuri_model_status status=enter(i,owner,generation);if(status)return status;
    add_count(&i->stats.eval_calls,1);
    bool same=i->evaluated;
    if(!camera||!light){i->evaluated=false;return leave(i,AYATSURI_MODEL_INVALID);}
    for(unsigned k=0;k<9;++k){if(!isfinite(camera[k])){i->evaluated=false;return leave(i,AYATSURI_MODEL_INVALID);}if(camera[k]!=i->camera[k])same=false;}
    for(unsigned k=0;k<6;++k){if(!isfinite(light[k])||(k>=3&&!unit(light[k]))){i->evaluated=false;return leave(i,AYATSURI_MODEL_INVALID);}if(light[k]!=i->light[k])same=false;}
    if(!(camera[6]>0)){i->evaluated=false;return leave(i,AYATSURI_MODEL_INVALID);}
    if(same)return leave(i,AYATSURI_MODEL_OK);
    i->evaluated=false;memcpy(i->camera,camera,36);memcpy(i->light,light,24);
    double fx=(double)camera[0]-camera[3],fy=(double)camera[1]-camera[4],fz=(double)camera[2]-camera[5];
    const double distance=hypot3(fx,fy,fz);
    if(!(distance>0)||!isfinite(distance))return leave(i,AYATSURI_MODEL_INVALID);
    fx/=distance;fy/=distance;fz/=distance;
    double rx=fz,rz=-fx,length=hypot(rx,rz);
    if(length<1e-6){rx=1;rz=0;length=1;}rx/=length;rz/=length;
    const float forward[3]={(float)fx,(float)fy,(float)fz},right[3]={(float)rx,0,(float)rz},
        down[3]={(float)(fy*rz),(float)(fz*rx-fx*rz),(float)(-fy*rx)};
    for(unsigned k=0;k<3;++k){i->lighting[k]=light[k];i->lighting[3+k]=forward[k];i->lighting[6+k]=light[3+k];}
    for(unsigned j=0;j<i->model->node_count;++j){
        const uint8_t *node=row(i->model,1,j,16);const int parent=parent_id(node);
        ayatsuri_pose pose;ayatsuri_transform p,result;memcpy(&pose,work(i,j)+12,36);
        if(parent>=0)memcpy(&p,work(i,(unsigned)parent),48);
        if(ayatsuri_transform_evaluate(&pose,parent<0?NULL:&p,&result)!=AYATSURI_OK)return leave(i,AYATSURI_MODEL_RANGE);
        add_count(&i->stats.geometry_updates,1);
        memcpy(work(i,j),&result,48);states(i)[j].world_enabled=states(i)[j].enabled&&(parent<0||states(i)[parent].world_enabled);
        if((node[2]==1&&!ellipse_packet(i,j,right,down))||(node[2]==2&&!blend_packet(i,j,right,down,forward)))
            return leave(i,AYATSURI_MODEL_RANGE);
        if(node[2]){add_count(&i->stats.projection_updates,1);add_count(&i->stats.shade_updates,1);}
    }
    i->evaluated=true;return leave(i,AYATSURI_MODEL_OK);
}
ayatsuri_model_status ayatsuri_model_instance_next(ayatsuri_model_instance *i,const void *owner,
        uint32_t generation,uint32_t *cursor,ayatsuri_model_packet *out){
    ayatsuri_model_status status=enter(i,owner,generation);if(status)return status;
    if(!i->evaluated)return leave(i,AYATSURI_MODEL_STATE);
    if(!cursor||!out||overlaps(cursor,sizeof *cursor,out,sizeof *out)||
       overlaps(cursor,sizeof *cursor,i,sizeof *i)||overlaps(cursor,sizeof *cursor,i->model,sizeof *i->model)||
       overlaps(cursor,sizeof *cursor,i->arena,i->arena_bytes)||overlaps(cursor,sizeof *cursor,i->model->data,i->model->bytes)||
       overlaps(out,sizeof *out,i,sizeof *i)||overlaps(out,sizeof *out,i->model,sizeof *i->model)||
       overlaps(out,sizeof *out,i->arena,i->arena_bytes)||overlaps(out,sizeof *out,i->model->data,i->model->bytes)||
       *cursor>i->model->node_count)return leave(i,AYATSURI_MODEL_INVALID);
    for(unsigned j=*cursor;j<i->model->node_count;++j){const uint8_t *node=row(i->model,1,j,16);
        if(node[2]&&states(i)[j].world_enabled){
            *out=(ayatsuri_model_packet){(const uint8_t *)work(i,j),i->lighting,(uint16_t)j,(uint16_t)slot_bytes(node),node[2],(uint8_t)m16(node+6)};
            *cursor=j+1;return leave(i,AYATSURI_MODEL_OK);
        }}
    *cursor=i->model->node_count;return leave(i,AYATSURI_MODEL_DONE);
}
