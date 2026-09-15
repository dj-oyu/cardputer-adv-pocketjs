/* Boundary 2a identity harness: one binary renders the same 120-frame script
 * once per arm of g_ksn_decode_once and compares the framebuffer hash and the
 * transfer counters render by render. The read counts are counted call sites in
 * the linked binary: the linker redirects every ksn_core_read reference from
 * ksn_render.c to __wrap_ksn_core_read (see run.sh), so the numbers are
 * measured, not estimated. The scalar reference is the arm with the switch off,
 * which is the pre-2a renderer: it reads the command again for every band and
 * twice for every group child. g_ksn_decode_once is defined weakly here so this
 * harness also links against a pre-2a ksn_render.c, where the variable does not
 * exist at all. Build it with -DKSN_PRE_DECODE_REUSE for that comparison: the
 * reference arm then reports the pre-2a read counts, the per-render contract
 * below cannot hold, and only the hashes and the transferred bytes are
 * compared. Without the define the contract is enforced.
 *
 * One transaction carries one layer, so a frame that touches both layers is a
 * SYSTEM submission plus an APP submission, each sealed and rendered in order;
 * every render then sees the whole bank (11 APP + 2 SYSTEM commands). */
#include "core_fixture.h"
#include "ksn_render.h"
#include <stdio.h>
#include <string.h>

#define CHECK(x) do{if(!(x)){fprintf(stderr,"decode reuse line %d: %s\n",__LINE__,#x);return 1;}}while(0)
#define SCRIT(x) do{if(!(x)){fprintf(stderr,"decode reuse line %d: %s\n",__LINE__,#x);return -1;}}while(0)
#define FRAMES 120u
#define RENDERS (FRAMES+32u) /* 120 frames, up to two renders each. */
#define APP_COMMANDS 11u
#define SYSTEM_COMMANDS 2u

int g_ksn_decode_once __attribute__((weak))=1;

static unsigned counted_reads;
ksn_result __real_ksn_core_read(const ksn_core *,ksn_tx,bool,ksn_layer,uint16_t,ksn_frame_command *);
ksn_result __wrap_ksn_core_read(const ksn_core *core,ksn_tx ticket,bool previous,ksn_layer layer,
                                uint16_t index,ksn_frame_command *out){
    counted_reads++;
    return __real_ksn_core_read(core,ticket,previous,layer,index,out);
}

KSN_TEST_CORE(core,static);
static uint16_t panel[240*135],strip[240*8];
static ksn_draw draws[APP_COMMANDS+SYSTEM_COMMANDS];
static ksn_ref refs[APP_COMMANDS+SYSTEM_COMMANDS];
static char pool[176]; /* Text bytes stay alive until the frame is submitted. */
static unsigned app_commands,system_commands;
static const uint32_t background=0x2b4b6bff;
static int fault_y=-1;

static uint16_t *buffer(void *ctx){(void)ctx;return strip;}
static ksn_result transfer(void *ctx,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)ctx;
    if(y==fault_y)return KSN_IO;
    memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));
    return KSN_OK;
}
/* Coverage depends on the counted bytes, on reveal and on the clip, so a cached
 * view that addressed the wrong frame text pool entry or kept a stale value
 * changes pixels instead of passing silently. */
static const unsigned inks[]={0,1,63,127,128,254,255};
static unsigned ink(int x,int y,uint16_t reveal,const ksn_draw *d){
    if(x<d->bounds.x0||x>=d->bounds.x1||y<d->bounds.y0||y>=d->bounds.y1||
       x<d->clip.x0||x>=d->clip.x1||y<d->clip.y0||y>=d->clip.y1)return 0;
    unsigned mask=0;
    for(unsigned i=0;i<d->data.text.bytes;i++)mask=mask*31u+(unsigned char)d->data.text.utf8[i];
    return inks[(mask+(unsigned)x*3u+(unsigned)y*5u+reveal*7u)%(sizeof(inks)/sizeof(inks[0]))];
}
static ksn_result span(void *ctx,const ksn_draw *d,uint16_t reveal,int x,int y,unsigned count,uint8_t *out){
    (void)ctx;
    /* Preflight passes count 0 and no buffer; the text pointer must still
     * address the frame's own copy of the counted bytes. */
    if(!d->data.text.utf8)return KSN_INVALID;
    for(unsigned i=0;i<count;i++)out[i]=(uint8_t)ink(x+(int)i,y,reveal,d);
    return KSN_OK;
}
static ksn_text_port text_port={.span=span};
static ksn_display_port display={NULL,buffer,transfer,240,135,8,&text_port};

static void glyphs(char *out,unsigned at,unsigned length){
    static const char set[]="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -#";
    for(unsigned i=0;i<length;i++)out[i]=set[(at*7u+i*3u)%(sizeof(set)-1u)];
}
/* Background, dithered gradient, two texts, a two-rect group, two round rects,
 * a stroke, two opaque rects (APP) and a SYSTEM notification pair. Every value
 * a PATCH changes is a function of the scene index, so a quiet frame is one
 * whose PATCH reproduces the previous frame exactly. */
static void build(unsigned scene){
    glyphs(pool,scene,64);glyphs(pool+64,scene+1,48);glyphs(pool+112,scene+2,40);
    draws[0]=(ksn_draw){.kind=KSN_RECT,.bounds={(int16_t)(4u+scene%16u),3,150,45},.clip={0,0,240,135},
                        .opacity=201,.data.shape={0xd9867243,0,0}};
    draws[1]=(ksn_draw){.kind=KSN_GRADIENT,.bounds={9,5,138,40},.clip={11,7,130,34},.opacity=219,
                        .data.gradient={0x25384900,0xe0a972d2,0,5,true}};
    draws[2]=(ksn_draw){.kind=KSN_TEXT,.bounds={6,6,120,20},.clip={0,0,240,135},.opacity=255,
                        .data.text={pool,24,80,KSN_BODY,0xa15f37b7}};
    draws[3]=(ksn_draw){.kind=KSN_TEXT,.bounds={10,24,132,40},.clip={0,0,240,135},.opacity=177,
                        .data.text={pool+64,9,80,KSN_CAPTION,0x3f7fbfc7}};
    draws[4]=(ksn_draw){.kind=KSN_RECT,.bounds={30,12,42,27},.clip={0,0,240,135},.opacity=255,
                        .data.shape={0x708ca4ff,0,0}};
    draws[5]=(ksn_draw){.kind=KSN_ROUND_RECT,.bounds={38,10,90,36},.clip={0,0,240,135},.opacity=177,
                        .data.shape={(0x357ecb00u|(scene*5u&0xffu)),6,0}};
    draws[6]=(ksn_draw){.kind=KSN_ROUND_RECT,.bounds={44,16,96,40},.clip={0,0,240,135},.opacity=147,
                        .data.shape={0xcbb57e93,3,0}};
    draws[7]=(ksn_draw){.kind=KSN_STROKE,.bounds={100,8,140,44},.clip={0,0,240,135},.opacity=255,
                        .data.shape={0x2f6f4fd1,0,1}};
    draws[8]=(ksn_draw){.kind=KSN_RECT,.bounds={146,3,236,60},.clip={0,0,240,135},.opacity=255,
                        .data.shape={0x9b6c2aff,0,0}};
    draws[9]=(ksn_draw){.kind=KSN_GRADIENT,.bounds={0,46,178,96},.clip={0,(int16_t)(scene%8u),240,135},
                        .opacity=255,.data.gradient={0x123456ff,0xffff80ff,0,0,true}};
    draws[10]=(ksn_draw){.kind=KSN_ROUND_RECT,.bounds={150,70,238,132},.clip={0,0,240,135},.opacity=255,
                         .data.shape={0x4080c0ff,8,0}};
    draws[11]=(ksn_draw){.kind=KSN_RECT,.bounds={0,0,240,8},.clip={0,0,240,135},.opacity=232,
                         .data.shape={0xff0000ff,0,0}};
    draws[12]=(ksn_draw){.kind=KSN_TEXT,.bounds={8,0,(int16_t)(200u+scene%32u),8},.clip={0,0,240,135},
                         .opacity=255,.data.text={pool+112,6,40,KSN_CAPTION,0xffffffff}};
}
static bool replaced(unsigned frame){return frame%8u==0u;}
/* Nothing changed at all: no command is rewritten and no band is damaged. */
static bool quiet(unsigned frame){return !replaced(frame)&&frame%37u==3u;}
/* Band 8 must exist in every retried frame, so these frames are invalidated. */
static bool faulted(unsigned frame){return frame%23u==7u;}
static unsigned hash_panel(void){
    uint64_t hash=1469598103934665603ull;
    const unsigned char *bytes=(const unsigned char *)panel;
    for(unsigned i=0;i<sizeof(panel);i++){hash^=bytes[i];hash*=1099511628211ull;}
    return (unsigned)(hash^(hash>>32));
}
/* Renders the sealed frame and returns the exact number of reads the
 * switched-on arm must make: one decode per command, none when no band was
 * damaged, and a second decode when IO failure forced a retry of the frame. */
static unsigned render_step(bool identical,bool retry,unsigned *hash,unsigned *reads,unsigned *bytes){
    unsigned commands=app_commands+system_commands;
    ksn_render_stats stats;counted_reads=0;fault_y=retry?8:-1;
    ksn_result result=ksn_render_rects(&core,&display,&stats);
    if(retry){
        CHECK(result==KSN_IO);
        fault_y=-1;
        CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    }else CHECK(result==KSN_OK);
    *reads=counted_reads;*bytes=stats.transferred_bytes;*hash=hash_panel();
    return identical?0u:(retry?2u*commands:commands);
}
/* 120 frames. A SYSTEM submission is rendered on its own before the APP
 * submission of the same frame, so frames that touch both layers still render
 * one bank holding all 13 commands. */
static int script(unsigned frame,bool retry,unsigned *hash,unsigned *reads,unsigned *bytes,unsigned *expected){
    ksn_client app=ksn_core_client(&core,KSN_APP),system=ksn_core_client(&core,KSN_SYSTEM);
    ksn_tx tx;ksn_change change;
    bool identical=quiet(frame);unsigned scene=identical?frame-1u:frame;unsigned step=0;
    if(retry)ksn_core_invalidate(&core);
    build(scene);
    if(frame%16u==0u){
        SCRIT(system.ops->begin(system.ctx,KSN_REPLACE,&tx)==KSN_OK);
        for(unsigned i=0;i<SYSTEM_COMMANDS;i++)SCRIT(system.ops->add(system.ctx,tx,&draws[APP_COMMANDS+i],&refs[APP_COMMANDS+i])==KSN_OK);
        SCRIT(system.ops->end(system.ctx,tx)==KSN_OK);system_commands=SYSTEM_COMMANDS;
        expected[step]=render_step(false,false,&hash[step],&reads[step],&bytes[step]);step++;
    }
    SCRIT(app.ops->begin(app.ctx,replaced(frame)?KSN_REPLACE:KSN_PATCH,&tx)==KSN_OK);
    if(replaced(frame)){
        SCRIT(app.ops->background(app.ctx,tx,background)==KSN_OK);
        for(unsigned i=0;i<APP_COMMANDS;i++)SCRIT(app.ops->add(app.ctx,tx,&draws[i],&refs[i])==KSN_OK);
        app_commands=APP_COMMANDS;
    }else{
        change=(ksn_change){.property=KSN_SET_RECT,.value.rect=draws[0].bounds};
        SCRIT(app.ops->change(app.ctx,tx,refs[0],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_COLOR,.value.color=draws[5].data.shape.color};
        SCRIT(app.ops->change(app.ctx,tx,refs[5],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_VISIBLE,.value.visible=(scene/5u)%2u==0};
        SCRIT(app.ops->change(app.ctx,tx,refs[6],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_REVEAL,.value.reveal=(uint16_t)(scene%25u)};
        SCRIT(app.ops->change(app.ctx,tx,refs[2],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_TEXT,.value.text={draws[3].data.text.utf8,9}};
        SCRIT(app.ops->change(app.ctx,tx,refs[3],&change)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_CLIP,.value.rect=draws[9].clip};
        SCRIT(app.ops->change(app.ctx,tx,refs[9],&change)==KSN_OK);
    }
    SCRIT(ksn_core_group(&core,KSN_APP,tx,refs[4],2,(uint8_t)(64u+scene%128u))==KSN_OK);
    SCRIT(app.ops->end(app.ctx,tx)==KSN_OK);
    expected[step]=render_step(identical,retry,&hash[step],&reads[step],&bytes[step]);step++;
    if(frame%16u==8u){
        SCRIT(system.ops->begin(system.ctx,KSN_PATCH,&tx)==KSN_OK);
        change=(ksn_change){.property=KSN_SET_RECT,.value.rect=draws[12].bounds};
        SCRIT(system.ops->change(system.ctx,tx,refs[12],&change)==KSN_OK);
        SCRIT(system.ops->end(system.ctx,tx)==KSN_OK);system_commands=SYSTEM_COMMANDS;
        expected[step]=render_step(false,false,&hash[step],&reads[step],&bytes[step]);step++;
    }
    return (int)step;
}
/* Runs the whole script with one arm. arm 0 is the switch on, 1 the pre-2a
 * reference, 2 alternates the switch at every render boundary. */
static int run(unsigned arm,unsigned *hashes,unsigned *reads,unsigned *bytes,unsigned *expected){
    unsigned step=0;
    memset(panel,0,sizeof(panel));memset(strip,0,sizeof(strip));
    ksn_core_init(&core);app_commands=system_commands=0;
    for(unsigned frame=0;frame<FRAMES;frame++){
        int count;
        if(arm==2)g_ksn_decode_once=(int)((step&1u)==0u);else g_ksn_decode_once=(int)(arm==0);
        count=script(frame,faulted(frame),hashes+step,reads+step,bytes+step,expected+step);
        if(count<0)return -1;
        step+=(unsigned)count;
    }
    return (int)step;
}
int main(void){
    static unsigned hashes[3][RENDERS],reads[3][RENDERS],bytes[3][RENDERS],expected[RENDERS];
    unsigned renders=0;
    for(unsigned arm=0;arm<3;arm++){
        int count=run(arm,hashes[arm],reads[arm],bytes[arm],expected);
        if(count<0||(arm&&(unsigned)count!=renders)){fprintf(stderr,"arm %u render count\n",arm);return 1;}
        renders=(unsigned)count;
    }
    g_ksn_decode_once=1;
    for(unsigned step=0;step<renders;step++){
        for(unsigned arm=1;arm<3;arm++){
            if(hashes[arm][step]!=hashes[0][step]){
                fprintf(stderr,"render %u: arm %u hash %08x, reference %08x\n",
                        step,arm,hashes[arm][step],hashes[0][step]);return 1;
            }
            if(bytes[arm][step]!=bytes[0][step]){
                fprintf(stderr,"render %u: arm %u transferred %u bytes, reference %u\n",
                        step,arm,bytes[arm][step],bytes[0][step]);return 1;
            }
        }
#ifndef KSN_PRE_DECODE_REUSE
        if(reads[0][step]!=expected[step]){
            fprintf(stderr,"render %u: switched-on reads %u, expected %u\n",
                    step,reads[0][step],expected[step]);return 1;
        }
        if(expected[step]&&reads[1][step]<=reads[0][step]){
            fprintf(stderr,"render %u: reference reads %u, switched-on %u\n",
                    step,reads[1][step],reads[0][step]);return 1;
        }
#endif
    }
    unsigned on=0,off=0,alternating=0,on_worst=0,off_worst=0,fault_on=0,fault_off=0,retries=0,full=RENDERS;
    uint64_t script=1469598103934665603ull;
    for(unsigned step=0;step<renders;step++){
        on+=reads[0][step];off+=reads[1][step];alternating+=reads[2][step];
        script^=hashes[0][step];script*=1099511628211ull;
        if(reads[0][step]>on_worst)on_worst=reads[0][step];
        if(reads[1][step]>off_worst)off_worst=reads[1][step];
        /* The contract names the renders, so this also holds pre-2a, where the
         * measured counts do not match it. */
        if(expected[step]==APP_COMMANDS+SYSTEM_COMMANDS&&full==RENDERS)full=step;
        if(expected[step]==2u*(APP_COMMANDS+SYSTEM_COMMANDS)){fault_on+=reads[0][step];fault_off+=reads[1][step];retries++;}
    }
#ifdef KSN_PRE_DECODE_REUSE
    printf("decode reuse: pre-2a renderer (no switch), %u frames, %u renders, %u commands\n",
           (unsigned)FRAMES,renders,APP_COMMANDS+SYSTEM_COMMANDS);
#else
    printf("decode reuse: %u frames, %u renders, 3 arms (on/off/alternating) byte-identical, %u commands\n",
           (unsigned)FRAMES,renders,APP_COMMANDS+SYSTEM_COMMANDS);
#endif
    printf("read calls: switched on=%u reference=%u ratio=%.1fx; worst render on=%u reference=%u\n",
           on,off,(double)off/(double)on,on_worst,off_worst);
    if(full<RENDERS)printf("render %u (first %u-command frame): switched on=%u reference=%u reads\n",
                           full,(unsigned)(APP_COMMANDS+SYSTEM_COMMANDS),reads[0][full],reads[1][full]);
    printf("the %u retried frames (2 decodes each): switched on=%u reference=%u reads; alternating arm total=%u\n",
           retries,fault_on,fault_off,alternating);
    /* The whole 120-frame pixel stream as one number, comparable across builds
     * of this harness (pre-2a, 2a on, sanitizers, -O2). */
    printf("script hash %016llx\n",(unsigned long long)script);
    return 0;
}
