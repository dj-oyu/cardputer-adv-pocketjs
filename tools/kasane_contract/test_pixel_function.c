#include "ksn_pixel_function.h"
#include "ksn_render.h"
#include "core_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static uint16_t strip[240*8],panel[240*135];
static unsigned send_calls,fail_on_send;
static uint16_t *get_strip(void *ctx){(void)ctx;return strip;}
static ksn_result send_strip(void *ctx,uint16_t y,uint16_t rows,
                             const uint16_t *pixels){
    (void)ctx;
    if(++send_calls==fail_on_send)return KSN_IO;
    memcpy(panel+(size_t)y*240,pixels,(size_t)rows*240*sizeof *pixels);
    return KSN_OK;
}

static ksn_pixel_instruction op(unsigned code,unsigned dst,unsigned a,
                                unsigned b,unsigned imm){
    return (ksn_pixel_instruction){(uint8_t)code,(uint8_t)dst,(uint8_t)a,
                                   (uint8_t)b,(uint16_t)imm};
}

static void test_pool(void){
    uint16_t storage[KSN_PIXEL_SLOTS][64];
    uint16_t *buffers[KSN_PIXEL_SLOTS]={storage[0],storage[1]};
    ksn_pixel_pool pool={0};
    assert(ksn_pixel_pool_init(&pool,8,8,buffers,64));
    ksn_image_port port;
    ksn_pixel_pool_port(&pool,&port);
    assert(port.read_span&&port.frames==KSN_PIXEL_SLOTS&&!port.opaque);
    uint16_t backdrop[64];
    for(unsigned i=0;i<64;i++)backdrop[i]=(uint16_t)(0x1000+i);
    ksn_pixel_underlay underlay={backdrop,64,8};
    ksn_pixel_frame frame={.width=8,.height=8,.count=2,
                           .color_reg=0,.alpha_reg=1};
    frame.code[0]=op(KSN_PIXEL_UNDERLAY,0,0,0,0);
    frame.code[1]=op(KSN_PIXEL_IMM,1,0,0,255);
    ksn_pixel_handle first={0},second={0};
    assert(ksn_pixel_pool_stage(&pool,&frame,&underlay,&first)==KSN_PIXEL_OK);
    assert(ksn_pixel_pool_stage(&pool,&frame,&underlay,&second)==KSN_PIXEL_BUSY);
    /* Neither builder nor source storage is retained by the bound provider. */
    backdrop[0]=0xf800;
    frame.code[0]=op(KSN_PIXEL_IMM,0,0,0,0x07e0);
    uint16_t color[8];uint8_t alpha[8];
    assert(port.read_span(port.ctx,0,first.slot,0,0,1,color,alpha)==KSN_OK);
    assert(color[0]==0x1000&&alpha[0]==255);
    assert(ksn_pixel_pool_finish(&pool,first,true,true));
    assert(ksn_pixel_pool_committed(&pool).generation==first.generation);
    assert(ksn_pixel_pool_stage(&pool,&frame,NULL,&second)==KSN_PIXEL_OK);
    assert(second.slot!=first.slot);
    assert(port.read_span(port.ctx,0,first.slot,0,0,1,color,alpha)==KSN_OK);
    assert(color[0]==0x1000);
    assert(port.read_span(port.ctx,0,second.slot,0,0,1,color,alpha)==KSN_OK);
    assert(color[0]==0x07e0);
    /* A failed LCD send keeps the candidate frozen for retry. A discarded
     * submission blocks stage until the old committed image is repaired. */
    assert(ksn_pixel_pool_stage(&pool,&frame,NULL,&first)==KSN_PIXEL_BUSY);
    assert(ksn_pixel_pool_finish(&pool,second,false,false));
    assert(port.read_span(port.ctx,0,second.slot,0,0,1,color,alpha)==KSN_INVALID);
    assert(ksn_pixel_pool_stage(&pool,&frame,&underlay,&second)==KSN_PIXEL_BUSY);
    assert(port.read_span(port.ctx,0,first.slot,0,0,1,color,alpha)==KSN_OK);
    ksn_pixel_pool_repair_done(&pool);
    assert(ksn_pixel_pool_stage(&pool,&frame,NULL,&second)==KSN_PIXEL_OK);
    assert(ksn_pixel_pool_finish(&pool,second,true,true));
    assert(port.read_span(port.ctx,0,first.slot,0,0,1,color,alpha)==KSN_INVALID);
    assert(port.read_span(port.ctx,0,second.slot,0,0,1,color,alpha)==KSN_OK);
    ksn_pixel_frame resized=frame;resized.width=7;
    assert(ksn_pixel_pool_stage(&pool,&resized,NULL,&first)==KSN_PIXEL_INVALID);
    assert(port.read_span(port.ctx,0,second.slot,0,0,1,color,alpha)==KSN_OK);
    frame.code[0]=op(KSN_PIXEL_UNDERLAY,0,0,0,0);
    backdrop[0]=0x3210;
    assert(ksn_pixel_pool_stage(&pool,&frame,&underlay,&first)==KSN_PIXEL_OK);
    assert(first.slot!=second.slot);
    backdrop[0]=0xffff;

    /* Run a staged frame through the actual compositor's I/O retry. Its
     * provider remains frozen while the failed submission is sealed. */
    KSN_TEST_CORE(core,static);ksn_core_init(&core);
    ksn_resource resource;
    assert(ksn_core_register_image(&core,KSN_APP,&port,&resource)==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_tx tx;ksn_ref ref;
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={10,5,18,13},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=resource,.frame=first.slot,.scale=KSN_IMAGE_1X}};
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&draw,&ref)==KSN_OK);
    ksn_result end_result=app.ops->end(app.ctx,tx);
    if(end_result!=KSN_OK)fprintf(stderr,"pool tx end=%d\n",end_result);
    assert(end_result==KSN_OK);
    ksn_display_port display={.strip=get_strip,.present=send_strip,
        .width=240,.height=135,.strip_rows=8};
    ksn_render_stats stats;
    send_calls=0;fail_on_send=2;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_IO);
    assert(send_calls==2);
    assert(panel[5*240+10]==0x3210);
    assert(panel[12*240+10]!=backdrop[56]);
    assert(ksn_pixel_pool_stage(&pool,&frame,&underlay,&second)==KSN_PIXEL_BUSY);
    assert(port.read_span(port.ctx,0,first.slot,0,0,1,color,alpha)==KSN_OK);
    assert(color[0]==0x3210);
    send_calls=0;fail_on_send=0;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(ksn_pixel_pool_finish(&pool,first,true,true));
    assert(panel[5*240+10]==0x3210&&panel[12*240+10]==backdrop[56]);
    uint16_t *overlap[KSN_PIXEL_SLOTS]={storage[0],storage[0]};
    assert(!ksn_pixel_pool_init(&pool,8,8,overlap,64));
    ksn_pixel_handle stale=first,fresh={0};
    assert(ksn_pixel_pool_init(&pool,8,8,buffers,64));
    ksn_pixel_underlay aliased={storage[0],64,8};
    assert(ksn_pixel_pool_stage(&pool,&frame,&aliased,&fresh)==KSN_PIXEL_INVALID);
    assert(ksn_pixel_pool_stage(&pool,&frame,&underlay,&fresh)==KSN_PIXEL_OK);
    assert(fresh.generation!=stale.generation);
    assert(!ksn_pixel_pool_finish(&pool,stale,true,true));
    assert(ksn_pixel_pool_finish(&pool,fresh,true,true));
    ksn_pixel_handle discarded={0};
    backdrop[0]=0x2222;
    assert(ksn_pixel_pool_stage(&pool,&frame,&underlay,&discarded)==KSN_PIXEL_OK);
    backdrop[0]=0x3333;
    assert(ksn_pixel_pool_finish(&pool,discarded,false,false));
    assert(ksn_pixel_pool_stage(&pool,&frame,&underlay,&discarded)==KSN_PIXEL_BUSY);
    assert(port.read_span(port.ctx,0,fresh.slot,0,0,1,color,alpha)==KSN_OK);
    assert(color[0]==0xffff);
    ksn_pixel_pool_repair_done(&pool);
    assert(ksn_pixel_pool_stage(&pool,&frame,&underlay,&discarded)==KSN_PIXEL_OK);
    assert(ksn_pixel_pool_finish(&pool,discarded,true,true));
}

static void test_retained_scene(void){
    ksn_pixel_pool pool={0};
    assert(ksn_pixel_pool_init(&pool,8,8,NULL,0));
    ksn_pixel_frame frame={.width=8,.height=8,.count=2,
                           .color_reg=0,.alpha_reg=1};
    frame.code[0]=op(KSN_PIXEL_IMM,0,0,0,0xf800);
    frame.code[1]=op(KSN_PIXEL_IMM,1,0,0,255);
    ksn_pixel_handle old={0},candidate={0};
    assert(ksn_pixel_pool_stage(&pool,&frame,NULL,&old)==KSN_PIXEL_OK);
    assert(ksn_pixel_pool_finish(&pool,old,true,true));
    ksn_image_port image_port;ksn_pixel_pool_port(&pool,&image_port);
    KSN_TEST_CORE(core,static);ksn_core_init(&core);
    ksn_resource resource;
    assert(ksn_core_register_image(&core,KSN_APP,&image_port,&resource)==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_tx tx;ksn_ref refs[2];
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={10,5,18,13},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=resource,.frame=old.slot,.scale=KSN_IMAGE_1X}};
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&draw,&refs[0])==KSN_OK);
    draw.bounds=(ksn_rect){30,5,38,13};
    assert(app.ops->add(app.ctx,tx,&draw,&refs[1])==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_display_port display={.strip=get_strip,.present=send_strip,
        .width=240,.height=135,.strip_rows=8};
    ksn_render_stats stats;
    send_calls=0;fail_on_send=0;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[5*240+10]==0xf800&&panel[5*240+30]==0xf800);

    frame.code[0]=op(KSN_PIXEL_IMM,0,0,0,0x07e0);
    assert(ksn_pixel_pool_stage(&pool,&frame,NULL,&candidate)==KSN_PIXEL_OK);
    ksn_change change={.property=KSN_SET_IMAGE_FRAME,
                       .value.image={0,candidate.slot}};
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    assert(app.ops->change(app.ctx,tx,refs[0],&change)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[5*240+10]==0x07e0&&panel[5*240+30]==0xf800);
    /* The second retained node still reads old. ACK is rejected while the
     * owner reports that not all old references have been retired. */
    assert(!ksn_pixel_pool_finish(&pool,candidate,true,false));
    assert(ksn_core_invalidate_image(&core,resource));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[5*240+10]==0x07e0&&panel[5*240+30]==0xf800);
    assert(app.ops->begin(app.ctx,KSN_PATCH,&tx)==KSN_OK);
    assert(app.ops->change(app.ctx,tx,refs[1],&change)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(ksn_pixel_pool_finish(&pool,candidate,true,true));
    uint16_t color;uint8_t alpha;
    assert(image_port.read_span(image_port.ctx,0,old.slot,0,0,1,
                                &color,&alpha)==KSN_INVALID);
    assert(ksn_core_invalidate_image(&core,resource));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[5*240+10]==0x07e0&&panel[5*240+30]==0x07e0);
}

static void test_max_work(void){
    static uint16_t storage[KSN_PIXEL_SLOTS][240*135];
    uint16_t *buffers[KSN_PIXEL_SLOTS]={storage[0],storage[1]};
    ksn_pixel_pool pool={0};
    assert(ksn_pixel_pool_init(&pool,240,135,buffers,240*135));
    ksn_pixel_frame frame={.width=240,.height=135,.count=18,
                           .color_reg=0,.alpha_reg=1};
    frame.code[0]=op(KSN_PIXEL_X,0,0,0,0);
    frame.code[1]=op(KSN_PIXEL_IMM,1,0,0,255);
    for(unsigned i=2;i<18;i++)frame.code[i]=op(KSN_PIXEL_ADD,0,0,0,0);
    ksn_pixel_handle handle={0};ksn_image_port port;
    assert(ksn_pixel_pool_stage(&pool,&frame,NULL,&handle)==KSN_PIXEL_OK);
    ksn_pixel_pool_port(&pool,&port);
    uint16_t color[240];uint8_t alpha[240];
    clock_t start=clock();
    uint32_t checksum=0;
    for(unsigned repeat=0;repeat<64;repeat++)for(unsigned y=0;y<135;y++){
        assert(port.read_span(port.ctx,0,handle.slot,(uint16_t)y,0,240,color,alpha)==KSN_OK);
        checksum+=color[239]+alpha[239];
    }
    double ms=1000.0*(double)(clock()-start)/CLOCKS_PER_SEC/64.0;
    printf("pixel max-work host: %.3f ms/frame for 240x135x18 (583200 op visits), checksum=%u\n",ms,checksum);
    printf("pixel pool memory: struct=%zu, max two underlays=%zu bytes\n",
           sizeof(ksn_pixel_pool),(size_t)2*240*135*sizeof(uint16_t));
    assert(ksn_pixel_pool_finish(&pool,handle,true,true));
}

static void test_small_scopes(void){
    const struct { uint16_t width,height; uint8_t commands; } scopes[]={
        {64,64,6},{112,63,8}
    };
    for(unsigned s=0;s<sizeof scopes/sizeof scopes[0];s++){
        unsigned width=scopes[s].width,height=scopes[s].height;
        unsigned commands=scopes[s].commands;
        ksn_pixel_pool pool={0};
        assert(ksn_pixel_pool_init(&pool,(uint16_t)width,(uint16_t)height,NULL,0));
        ksn_pixel_frame frame={.width=(uint16_t)width,.height=(uint16_t)height,
                               .count=(uint8_t)commands,.color_reg=0,.alpha_reg=1};
        frame.code[0]=op(KSN_PIXEL_X,0,0,0,0);
        frame.code[1]=op(KSN_PIXEL_IMM,1,0,0,255);
        for(unsigned i=2;i<commands;i++)frame.code[i]=op(KSN_PIXEL_ADD,0,0,0,0);
        ksn_pixel_handle handle={0};ksn_image_port port;
        assert(ksn_pixel_pool_stage(&pool,&frame,NULL,&handle)==KSN_PIXEL_OK);
        ksn_pixel_pool_port(&pool,&port);
        uint16_t expected=(uint16_t)((width-1u)<<(commands-2u));
        uint16_t color[240];uint8_t alpha[240];
        uint32_t checksum=0;
        clock_t start=clock();
        for(unsigned repeat=0;repeat<64;repeat++)for(unsigned y=0;y<height;y++){
            assert(port.read_span(port.ctx,0,handle.slot,(uint16_t)y,0,
                                  (uint16_t)width,color,alpha)==KSN_OK);
            assert(color[0]==0&&color[width-1]==expected&&alpha[width-1]==255);
            checksum+=color[width-1]+alpha[width-1];
        }
        assert(checksum==(uint32_t)64*height*(expected+255u));
        printf("pixel scope host: %ux%ux%u %.3f ms/frame checksum=%u\n",
               width,height,commands,
               1000.0*(double)(clock()-start)/CLOCKS_PER_SEC/64.0,checksum);
        assert(ksn_pixel_pool_finish(&pool,handle,true,true));
    }
}

int main(void){
    /* Explicit underlay read, coordinate-dependent color, and transparent
     * alternate rows. The source array acts as an immutable captured layer. */
    uint16_t backdrop[8*8];
    for(unsigned y=0;y<8;y++)for(unsigned x=0;x<8;x++)
        backdrop[y*8+x]=(uint16_t)(0x1000+y*16+x);
    ksn_pixel_underlay underlay={backdrop,64,8};
    ksn_pixel_frame source={.width=8,.height=8,.count=8,
                            .color_reg=5,.alpha_reg=7};
    source.params[0]=0x001f;
    source.code[0]=op(KSN_PIXEL_UNDERLAY,0,0,0,0);
    source.code[1]=op(KSN_PIXEL_PARAM,1,0,0,0);
    source.code[2]=op(KSN_PIXEL_X,2,0,0,0);
    source.code[3]=op(KSN_PIXEL_XOR,3,0,1,0);
    source.code[4]=op(KSN_PIXEL_ADD,5,3,2,0);
    source.code[5]=op(KSN_PIXEL_Y,4,0,0,0);
    source.code[6]=op(KSN_PIXEL_IMM,6,0,0,255);
    source.code[7]=op(KSN_PIXEL_MUL,7,4,6,0);

    ksn_pixel_image image={0};
    ksn_image_port port;
    ksn_pixel_image_port(&image,&port);
    assert(!port.read_span);
    assert(ksn_pixel_image_bind(&image,&source,&underlay));
    ksn_pixel_image_port(&image,&port);
    assert(port.read_span&&!port.opaque&&port.width==8&&port.height==8);
    /* Builder mutation after bind cannot change the frozen frame. */
    source.params[0]=0xf800;
    source.code[4]=op(KSN_PIXEL_IMM,5,0,0,0);
    uint16_t color[8],second[8];uint8_t alpha[8],second_alpha[8];
    for(int y=7;y>=0;y--){
        assert(port.read_span(port.ctx,0,0,(uint16_t)y,0,8,color,alpha)==KSN_OK);
        for(unsigned x=0;x<8;x++){
            assert(color[x]==(uint16_t)((backdrop[y*8+x]^0x001f)+x));
            assert(alpha[x]==(y?255:0));
        }
    }
    assert(port.read_span(port.ctx,0,0,4,2,4,color,alpha)==KSN_OK);
    assert(port.read_span(port.ctx,0,0,4,2,4,second,second_alpha)==KSN_OK);
    assert(!memcmp(color,second,4*sizeof *color));
    assert(!memcmp(alpha,second_alpha,4));
    assert(port.read_span(port.ctx,0,0,4,8,0,NULL,NULL)==KSN_OK);
    assert(port.read_span(port.ctx,0,0,8,0,1,color,alpha)==KSN_INVALID);
    assert(port.read_span(port.ctx,0,0,4,7,2,color,alpha)==KSN_INVALID);

    /* Bad candidate cannot replace the old bound image. */
    ksn_pixel_frame bad=source;
    bad.code[4]=op(KSN_PIXEL_ADD,5,0,8,0);
    assert(!ksn_pixel_image_bind(&image,&bad,&underlay));
    assert(port.read_span(port.ctx,0,0,1,0,1,color,alpha)==KSN_OK);
    assert(color[0]==(uint16_t)(backdrop[8]^0x001f));
    bad=source;bad.code[4]=op(KSN_PIXEL_IMM,5,0,0,0);
    assert(!ksn_pixel_image_bind(&image,&bad,NULL));
    ksn_pixel_underlay short_underlay={backdrop,63,8};
    assert(!ksn_pixel_image_bind(&image,&bad,&short_underlay));
    bad=(ksn_pixel_frame){.width=240,.height=135,.count=24,
                          .color_reg=0,.alpha_reg=0};
    for(unsigned i=0;i<bad.count;i++)bad.code[i]=op(KSN_PIXEL_IMM,0,0,0,1);
    assert(!ksn_pixel_image_bind(&image,&bad,NULL));
    bad=source;bad.code[0]=op(KSN_PIXEL_ADD,0,0,1,0);
    assert(!ksn_pixel_image_bind(&image,&bad,&underlay));

    /* A later safe bind refreshes the row cache, including the same y. */
    ksn_pixel_frame next={.width=8,.height=8,.count=2,
                          .color_reg=0,.alpha_reg=1};
    next.code[0]=op(KSN_PIXEL_IMM,0,0,0,0x07e0);
    next.code[1]=op(KSN_PIXEL_IMM,1,0,0,255);
    assert(ksn_pixel_image_bind(&image,&next,NULL));
    assert(port.read_span(port.ctx,0,0,1,0,1,color,alpha)==KSN_OK);
    assert(color[0]==0x07e0&&alpha[0]==255);
    KSN_TEST_CORE(core,static);ksn_core_init(&core);
    ksn_resource resource;
    assert(ksn_core_register_image(&core,KSN_APP,&port,&resource)==KSN_OK);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_tx tx;ksn_ref ref;
    ksn_draw draw={.kind=KSN_IMAGE,.bounds={10,5,18,13},
        .clip={0,0,240,135},.opacity=255,
        .data.image={.resource=resource,.scale=KSN_IMAGE_1X}};
    assert(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    assert(app.ops->background(app.ctx,tx,0x000000ff)==KSN_OK);
    assert(app.ops->add(app.ctx,tx,&draw,&ref)==KSN_OK);
    assert(app.ops->end(app.ctx,tx)==KSN_OK);
    ksn_display_port display={.strip=get_strip,.present=send_strip,
        .width=240,.height=135,.strip_rows=8};
    ksn_render_stats stats;
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[5*240+10]==0x07e0&&panel[12*240+17]==0x07e0);
    /* A safe bind after ACK and invalidation repaints through the image port. */
    next.code[0]=op(KSN_PIXEL_IMM,0,0,0,0xf800);
    assert(ksn_pixel_image_bind(&image,&next,NULL));
    assert(ksn_core_invalidate_image(&core,resource));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[5*240+10]==0xf800&&panel[12*240+17]==0xf800);
    ksn_pixel_frame resized=next;
    resized.width=4;resized.height=4;
    assert(!ksn_pixel_image_bind(&image,&resized,NULL));
    assert(ksn_core_invalidate_image(&core,resource));
    assert(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    assert(panel[5*240+10]==0xf800&&panel[12*240+17]==0xf800);
    test_pool();
    test_retained_scene();
    test_max_work();
    test_small_scopes();
    puts("pixel function: bounded IR, frozen frame/underlay, candidate lease, band reread passed");
    return 0;
}
