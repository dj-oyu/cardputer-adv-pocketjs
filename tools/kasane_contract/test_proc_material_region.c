/* Surface-coordinate oracle and old-packet golden; no device timing claims. */
#define main legacy_ellipsoid_main
#include "test_proc_ellipsoid.c"
#undef main
#include <stdint.h>
static uint32_t old_modes(void){
    uint32_t hash=2166136261u;
    for(unsigned mode=0;mode<4;++mode)for(unsigned j=0;j<5;++j){
        ksn_proc_ellipsoid p=sample();p.surface[4]=(uint16_t)mode;
        p.screen[0]=20+3*j;p.screen[1]=2;p.screen[4]=-3;p.screen[5]=15+2*j;
        p.texture[7]=.2f;p.texture[8]=.8f;p.material[0]=.3f;p.material[1]=.7f;p.material[2]=.4f;
        p.surface[2]=(uint16_t)(255-25*j);p.light[0]=.3f;p.light[1]=-.2f;
        put(&p);draw(0x2137);
        for(unsigned k=0;k<KSN_PROC_W*KSN_PROC_H;++k){hash=(hash^(full[k]&255))*16777619u;hash=(hash^(full[k]>>8))*16777619u;}
    }
    return hash;
}
#ifndef REGION_GOLDEN_ONLY
static void capture(unsigned index){
    const char *folder=getenv("REGION_CAPTURE_DIR");if(!folder)return;
    char path[1024];snprintf(path,sizeof path,"%s/app_%04u.ppm",folder,index);
    FILE *f=fopen(path,"wb");CHECK(f);fprintf(f,"P6\n240 135\n255\n");
    for(unsigned k=0;k<240*135;++k){const uint16_t c=full[k];
        fputc(((c>>11)&31)*255/31,f);fputc(((c>>5)&63)*255/63,f);fputc((c&31)*255/31,f);}
    CHECK(!fclose(f));
}
static ksn_proc_ellipsoid region(void){
    ksn_proc_ellipsoid p=sample();p.screen[0]=46;p.screen[3]=120;p.screen[5]=38;p.screen[7]=68;
    p.normal[0]=1.0f/46;p.normal[4]=1.0f/38;p.normal[8]=1.0f/30;
    p.surface[0]=0xe3c6;p.surface[1]=0xff37;p.surface[4]=4;
    p.texture[0]=.6f;p.texture[1]=.8f;p.texture[2]=0;p.texture[3]=.05f;p.texture[4]=.08f;
    p.material[0]=.3f;p.material[1]=.65f;p.material[2]=.5f;p.material[3]=24;
    p.light[0]=-.5f;p.light[1]=-.4f;p.light[2]=.8f;return p;
}
#endif
int main(void){
    const uint32_t golden=old_modes();printf("MATERIAL_OLD_GOLDEN %08x\n",golden);
#ifdef REGION_GOLDEN_ONLY
    return 0;
#else
    /* Filled from the baseline b93 renderer using the identical 20-packet fixture. */
    CHECK(golden==0xc66daae5u);
    ksn_proc_ellipsoid p=region();
    for(unsigned j=0;j<3;++j){
        const double angle=(double)j*.37,c=cos(angle),s=sin(angle),rx=30+7*j,ry=20+3*j;
        p.screen[0]=(float)(rx*c);p.screen[1]=(float)(-ry*s);p.screen[4]=(float)(rx*s);p.screen[5]=(float)(ry*c);
        p.texture[4]=0;p.material[0]=1;p.material[1]=p.material[2]=p.material[4]=0;
        put(&p);draw(0x2137);
        for(int y=0;y<135;++y)for(int x=0;x<240;++x){
            const double qx=(c*(x-120)+s*(y-68))/rx,qy=(-s*(x-120)+c*(y-68))/ry;
            const double residual=1-qx*qx-qy*qy,d=.6f*qx+.8f*qy-.05f;
            if(residual>.2&&fabs(d)>1e-4)CHECK(full[y*240+x]==(d>=0?p.surface[1]:p.surface[0]));
        }
        memcpy(strips,full,sizeof full);p.light[0]=.7f;p.light[1]=.2f;p.light[2]=-.4f;
        put(&p);for(unsigned k=0;k<240*135;++k)full[k]=0x2137;CHECK(ksn_proc_render_band(&frame,full,0,135));
        CHECK(!memcmp(full,strips,sizeof full));
    }
    p=region();p.surface[4]=0;put(&p);draw(0x2137);capture(0);
    p.surface[4]=2;p.texture[0]=1;p.texture[1]=0;p.texture[4]=1;p.texture[7]=.55f;p.texture[8]=1;put(&p);draw(0x2137);capture(1);
    p=region();p.texture[4]=0;put(&p);draw(0x2137);capture(2);
    p=region();put(&p);draw(0x2137);capture(3);
    p.light[0]=.6f;p.light[1]=.2f;p.light[2]=.7f;put(&p);draw(0x2137);capture(4);
    p=region();p.screen[0]=40;p.screen[1]=-19;p.screen[4]=23;p.screen[5]=33;
    p.normal[0]=.8660254f/46;p.normal[1]=-.5f/38;p.normal[3]=.5f/46;p.normal[4]=.8660254f/38;
    put(&p);draw(0x2137);capture(5);
    p=region();p.screen[0]=60;p.screen[5]=25;p.normal[0]=1.0f/60;p.normal[4]=1.0f/25;put(&p);draw(0x2137);capture(6);
    p=region();p.surface[0]=p.surface[1]=0;p.material[0]=p.material[1]=0;p.material[2]=1;p.material[3]=8;
    p.surface[4]=0;put(&p);draw(0x2137);capture(7);memcpy(strips,full,sizeof full);
    p.surface[4]=4;put(&p);for(unsigned k=0;k<240*135;++k)full[k]=0x2137;CHECK(ksn_proc_render_band(&frame,full,0,135));
    CHECK(!memcmp(full,strips,sizeof full));capture(8);
    for(unsigned j=0;j<=60;++j){
        const float angle=(-35+70*(float)j/60)*.0174532925199433f,c=cosf(angle),s=sinf(angle);
        p=region();p.screen[0]=46*c;p.screen[2]=30*s;
        p.normal[0]=c/46;p.normal[2]=s/30;p.normal[6]=-s/46;p.normal[8]=c/30;
        put(&p);draw(0x2137);capture(9+j);
    }
    p=region();put(&p);ksn_proc_surface_init(&surface);
    uint32_t generation=ksn_proc_surface_stage(&surface,&frame);CHECK(generation);CHECK(ksn_proc_surface_finish(&surface,generation,true));
    for(unsigned field=0;field<3;++field){
        if(field==0)p.texture[3]+=.1f;else if(field==1)p.texture[4]+=.1f;else p.surface[1]^=0x07e0;
        put(&p);generation=ksn_proc_surface_stage(&surface,&frame);CHECK(generation);
        CHECK(ksn_proc_surface_pending_damage(&surface)->bands);CHECK(ksn_proc_surface_finish(&surface,generation,true));
    }
    p.texture[3]+=.1f;put(&p);generation=ksn_proc_surface_stage(&surface,&frame);CHECK(generation);
    CHECK(ksn_proc_surface_finish(&surface,generation,false));CHECK(ksn_proc_surface_needs_repair(&surface));
    CHECK(!ksn_proc_surface_stage(&surface,&frame));ksn_proc_surface_repair_done(&surface);
    generation=ksn_proc_surface_stage(&surface,&frame);CHECK(generation);CHECK(ksn_proc_surface_finish(&surface,generation,true));
    puts("MATERIAL_REGION_PASS oldGolden/bodyLocal/nonuniform/lightInvariant/specularSame/bands");return 0;
#endif
}
