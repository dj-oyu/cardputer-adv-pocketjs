/* Common-surface albedo regions; old renderer fixtures remain the oracle. */
#define main legacy_blend_main
#include "test_proc_blend_ellipsoids.c"
#undef main
static uint16_t original[KSN_PROC_W*KSN_PROC_H];
static ksn_proc_frame saved;
static void put_region(const ksn_proc_blend_ellipsoids *p,const ksn_proc_blend_region_mask *mask){
    int l,t,r,b;CHECK(ksn_proc_blend_region_bounds(p,mask,&l,&t,&r,&b));CHECK(l<=r&&t<=b);
    const unsigned words=(unsigned)KSN_PROC_BLEND_REGION_WORDS(p->header.count);
    frame=(ksn_proc_frame){.count=(uint16_t)(words+1),.raster_steps=(uint16_t)((r-l+1)*(b-t+1)*p->header.count),.ext=1,.ready=true};
    frame.segments[0]=(ksn_proc_segment){(int16_t)(l+KSN_PROC_EXT_BLEND_REGION),(int16_t)t,(int16_t)r,(int16_t)b,0};
    const size_t bytes=KSN_PROC_BLEND_BYTES(p->header.count);
    memcpy(frame.segments+1,p,bytes);memcpy((uint8_t *)(frame.segments+1)+bytes,mask,sizeof *mask);
}
static void decode_bounds(void){
    for(unsigned n=1;n<=8;n++){
        ksn_proc_blend_ellipsoids p=sample();for(unsigned k=2;k<n;k++)p.lobes[k]=p.lobes[k%2];p.header.count=n;
        ksn_proc_blend_region_mask mask={.plane={0,0,1,-4},.color=0xf800};put_region(&p,&mask);
        const unsigned words=frame.count-1;ksn_proc_blend_ellipsoids q;ksn_proc_blend_region_mask out;unsigned used;
        for(unsigned cut=0;cut<words;cut++)CHECK(!ksn_proc_blend_region_read(frame.segments+1,cut,&q,&out,&used));
        CHECK(ksn_proc_blend_region_read(frame.segments+1,words,&q,&out,&used));CHECK(used==words);
        CHECK(!memcmp(&p,&q,KSN_PROC_BLEND_BYTES(n))&&!memcmp(&mask,&out,sizeof mask));
        unsigned cursor=0,cost;ksn_proc_segment g;bool ext;
        CHECK(ksn_proc_frame_next(&frame,&cursor,&g,&ext,&cost));CHECK(ext&&cursor==frame.count&&cost==frame.raster_steps);
        frame.segments[0].x1--;cursor=0;CHECK(!ksn_proc_frame_next(&frame,&cursor,&g,&ext,&cost)&&cursor>frame.count);
    }
    ksn_proc_blend_ellipsoids p=sample();int l,t,r,b;
    const ksn_proc_blend_region_mask good={.plane={1,0,0,-40},.feather=2,.color=0xffff};
    for(unsigned k=0;k<7;k++){
        ksn_proc_blend_region_mask mask=good;
        if(k<4)mask.plane[k]=k==3?1e6f+1:NAN;
        else if(k==4)mask.feather=1e-5f;else if(k==5)mask.feather=4097;else mask.reserved=1;
        p.header.surface[1]=0;CHECK(!ksn_proc_blend_region_bounds(&p,&mask,&l,&t,&r,&b));
    }
    ksn_proc_blend_region_mask zero={0};CHECK(!ksn_proc_blend_region_bounds(&p,&zero,&l,&t,&r,&b));
}
/* Independent binary64 smooth-max depth for the sample's two circular lobes.
 * Interior pixels avoid mixing this material test with silhouette coverage. */
static void depth_oracle(void){
    ksn_proc_blend_ellipsoids p=sample();p.header.surface[0]=0xf800;
    p.header.material[0]=1;p.header.material[1]=0;
    ksn_proc_blend_region_mask mask={.plane={.3f,-.1f,.7f,-17},.color=0x001f};
    for(unsigned soft=0;soft<2;soft++){
        mask.feather=soft?5:0;put_region(&p,&mask);draw(0x07e0);unsigned checked=0;
        for(int y=20;y<=40;y++)for(int x=24;x<=56;x++){
            bool found=false;double depth=0,cover=0;
            for(unsigned k=0;k<2;k++){
                const double dx=(x-(33+14*(int)k))/12.0,dy=(y-30)/12.0,residual=1-dx*dx-dy*dy;
                if(residual<=0)continue;
                const double d=10*sqrt(residual);cover=fmax(cover,residual/(2.0/12));
                if(!found){depth=d;found=true;}else{const double w=fmax(0,fmin(1,.5+.5*(d-depth)/8));depth=depth*(1-w)+d*w+8*w*(1-w);}
            }
            if(!found||cover<1.001)continue;
            const double distance=(double)mask.plane[0]*x+(double)mask.plane[1]*y+(double)mask.plane[2]*depth+mask.plane[3];
            if(fabs(distance)<1e-3)continue;
            const double mix=soft?fmax(0,fmin(1,.5+distance/5)):(distance>=0?1:0);
            const uint16_t expected=(uint16_t)((unsigned)lround(31*(1-mix))<<11)|(uint16_t)lround(31*mix);
            CHECK(full[y*240+x]==expected);checked++;
        }
        CHECK(checked>200);
    }
}
static void colour_and_lifetime(void){
    ksn_proc_blend_ellipsoids p=sample();p.header.material[2]=.4f;p.header.surface[1]=181;
    ksn_proc_blend_region_mask mask={.plane={.4f,-.1f,.3f,-20},.feather=3,.color=p.header.surface[0]};
    put(&p);draw(0x1234);memcpy(original,full,sizeof full);
    put_region(&p,&mask);draw(0x1234);CHECK(!memcmp(original,full,sizeof full));
    p.header.material[0]=p.header.material[1]=0;mask.color=0xf800;
    put(&p);draw(0x1234);memcpy(original,full,sizeof full);
    put_region(&p,&mask);draw(0x1234);CHECK(!memcmp(original,full,sizeof full));
    ksn_proc_surface_init(&surface);uint32_t generation=ksn_proc_surface_stage(&surface,&frame);CHECK(generation);
    memset(frame.segments,0,sizeof frame.segments);CHECK(ksn_proc_surface_finish(&surface,generation,true));
    const ksn_proc_frame *committed=ksn_proc_surface_committed_frame(&surface);CHECK(committed&&committed->segments[0].x0>=KSN_PROC_EXT_BLEND_REGION);
    saved=*committed;
    for(unsigned field=0;field<6;field++){
        ksn_proc_blend_region_mask changed=mask;
        if(field<4)changed.plane[field]+=.05f;else if(field==4)changed.feather+=1;else changed.color++;
        put_region(&p,&changed);generation=ksn_proc_surface_stage(&surface,&frame);CHECK(generation);
        CHECK(ksn_proc_surface_pending_damage(&surface)->bands!=0);
        CHECK(ksn_proc_surface_finish(&surface,generation,false));CHECK(ksn_proc_surface_needs_repair(&surface));
        CHECK(!memcmp(ksn_proc_surface_committed_frame(&surface),&saved,sizeof saved));
        CHECK(!ksn_proc_surface_stage(&surface,&frame));ksn_proc_surface_repair_done(&surface);
    }
}
int main(void){legacy_blend_main();decode_bounds();depth_oracle();colour_and_lifetime();
    puts("PASS blend common-plane final-depth/albedo/same-colour/specular/bounds/snapshot/repair");return 0;}
