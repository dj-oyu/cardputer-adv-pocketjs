#include "core_fixture.h"
#include "ksn_render.h"
#ifdef KSN_PROF_ABSENT
/* Before boundary 7a the render path has no counters and no switch. The scene
 * below is the same in both builds, so the pixel hash this file prints from the
 * baseline tree and from the instrumented tree is a like-for-like comparison:
 * "the switch off" is not "the same code", it is this number and that one
 * agreeing byte for byte. */
#define KSN_PROF_REPORT 0
#else
#define KSN_PROF_REPORT 1
#endif
#include <stdio.h>
#include <string.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"render prof line %d: %s\n",__LINE__,#x);return 1;}} while(0)

static uint16_t panel[240*135],strip[240*8];
#if KSN_PROF_REPORT
static uint16_t panel_off[240*135];
#endif
static unsigned sends;
static uint16_t *buffer(void *p){(void)p;return strip;}
static ksn_result send(void *p,uint16_t y,uint16_t rows,const uint16_t *pixels){
    (void)p;sends++;memcpy(panel+y*240,pixels,rows*240*sizeof(*pixels));return KSN_OK;
}
/* Synthetic coverage, the shape test_text_render.c uses: these counters are
 * about the render path's phases, not about a font's ink. count=0 is the
 * preflight probe and must still answer KSN_OK. */
static unsigned coverage(int x,int y){
    static const unsigned values[]={0,1,127,128,254,255};
    if(x<2||x>=77||y<7||y>=22)return 0;
    return values[(x+y)%6];
}
static ksn_result span(void *p,const ksn_draw *d,uint16_t reveal,int x,int y,unsigned n,uint8_t *out){
    (void)p;(void)reveal;
    CHECK(d->kind==KSN_TEXT&&n<=64);
    for(unsigned i=0;i<n;i++)out[i]=(uint8_t)coverage(x+(int)i,y);
    return KSN_OK;
}
static uint32_t hash_panel(void){
    uint32_t h=2166136261u;
    const uint8_t *bytes=(const uint8_t *)panel;
    for(unsigned i=0;i<sizeof(panel);i++){h^=bytes[i];h*=16777619u;}
    return h;
}
/* One REPLACE frame that enters every phase the counters bracket: the band
 * background and an opaque rect (fill), a dithered gradient and text ink
 * (blend), a text span (span), a two-child group (tile, which contains the
 * reads, spans and per-pixel composite of its children), and the command scan
 * of every band (read). Each draw's union is zeroed first: a ksn_draw carries
 * leftover bytes from the previous kind otherwise, and ksn_core.c reads the
 * whole payload. */
static int render_once(int prof_on,uint32_t *hash){
#if KSN_PROF_REPORT
    g_ksn_prof=prof_on;
#else
    (void)prof_on;
#endif
    KSN_TEST_CORE(core,);ksn_core_init(&core);
    ksn_client app=ksn_core_client(&core,KSN_APP);
    ksn_text_port text={.span=span};
    ksn_display_port display={NULL,buffer,send,240,135,8,&text,NULL};
    ksn_tx tx;ksn_ref rect,grad,label,shape,ring;
    ksn_render_stats stats;
    ksn_draw d={.clip={0,0,240,135},.opacity=255};
    CHECK(app.ops->begin(app.ctx,KSN_REPLACE,&tx)==KSN_OK);
    CHECK(app.ops->background(app.ctx,tx,0x0b1727ff)==KSN_OK);
    d.kind=KSN_RECT;d.bounds=(ksn_rect){2,2,40,20};
    memset(&d.data,0,sizeof(d.data));d.data.shape.color=0x67dfc7ff;
    CHECK(app.ops->add(app.ctx,tx,&d,&rect)==KSN_OK);
    d.kind=KSN_GRADIENT;d.bounds=(ksn_rect){10,30,110,60};
    memset(&d.data,0,sizeof(d.data));
    d.data.gradient.from=0xff0000ff;d.data.gradient.to=0x0000ffff;
    d.data.gradient.axis=1;d.data.gradient.dither=true;d.data.gradient.radius=6;
    CHECK(app.ops->add(app.ctx,tx,&d,&grad)==KSN_OK);
    d.kind=KSN_TEXT;d.bounds=(ksn_rect){-3,5,145,29};d.clip=(ksn_rect){2,7,77,22};
    memset(&d.data,0,sizeof(d.data));
    d.data.text.utf8="\xe3\x81\x82" "A";d.data.text.bytes=4;d.data.text.capacity=16;
    d.data.text.font=KSN_BODY;d.data.text.color=0xa15f37b7;
    CHECK(app.ops->add(app.ctx,tx,&d,&label)==KSN_OK);
    d.clip=(ksn_rect){0,0,240,135};
    d.kind=KSN_ROUND_RECT;d.bounds=(ksn_rect){150,30,180,60};d.opacity=200;
    memset(&d.data,0,sizeof(d.data));d.data.shape.radius=8;d.data.shape.color=0x3fa9f5ff;
    CHECK(app.ops->add(app.ctx,tx,&d,&shape)==KSN_OK);
    d.kind=KSN_STROKE;d.bounds=(ksn_rect){190,30,235,70};d.opacity=255;
    memset(&d.data,0,sizeof(d.data));d.data.shape.width=2;d.data.shape.color=0xffffff80;
    CHECK(app.ops->add(app.ctx,tx,&d,&ring)==KSN_OK);
    CHECK(ksn_core_group(&core,KSN_APP,tx,shape,2,137)==KSN_OK);
    CHECK(app.ops->end(app.ctx,tx)==KSN_OK);
    sends=0;
    CHECK(ksn_render_rects(&core,&display,&stats)==KSN_OK);
    CHECK(stats.bands==0x1ffffu&&stats.transferred_bytes==64800u&&sends==17);
    *hash=hash_panel();
    return 0;
}
int main(void){
#if KSN_PROF_REPORT
    uint32_t off=0,on=0;
    {
        ksn_render_prof prof,zero={0},drained;
        CHECK(render_once(0,&off)==0);
        CHECK(!memcmp(&g_ksn_render_prof,&zero,sizeof(zero)));
        memcpy(panel_off,panel,sizeof(panel));
        CHECK(render_once(1,&on)==0);
        prof=g_ksn_render_prof;
        CHECK(memcmp(panel_off,panel,sizeof(panel))==0);
        /* Counts really counted, not merely non-zero: 17 damaged bands each put
         * one background fill565 through the bracket (17), and the opaque rect
         * is 18 rows inside three of them (fill = one call, so 18 more). The
         * text is one preflight span plus 15 clipped rows x 2 64-pixel blocks
         * (31): that one is ink-independent, which is why a count and not a
         * painted-pixel number is the thing to contract. */
        CHECK(prof.fill_n==35u&&prof.span_n==31u);
        const unsigned want[]={prof.fill_n,prof.span_n,prof.tile_n,prof.blend_n,prof.read_n};
        for(unsigned i=0;i<5;i++)CHECK(want[i]!=0);
        /* The group is entered once per damaged band, including the ones it has
         * no rows in: that is the pass kasane-opt-survey.md 4 calls the 17x
         * cost of a two-command group, and it is a count this bracket has to
         * reproduce. */
        CHECK(prof.tile_n==17u);
        printf("RENDER_PROF pixels_off=0x%08x pixels_on=0x%08x identical=%d "
               "fill=%u/%u span=%u/%u tile=%u/%u blend=%u/%u read=%u/%u\n",
               off,on,memcmp(panel_off,panel,sizeof(panel))==0,
               prof.fill_n,prof.fill_cy,prof.span_n,prof.span_cy,prof.tile_n,prof.tile_cy,
               prof.blend_n,prof.blend_cy,prof.read_n,prof.read_cy);
        ksn_render_prof_read(&drained);
        CHECK(!memcmp(&drained,&prof,sizeof(prof))); /* the snapshot is what ran */
        CHECK(!memcmp(&g_ksn_render_prof,&zero,sizeof(zero))); /* and it resets */
    }
    /* Band mask helpers: the log line's band count and contiguous runs come
     * from ksn_render_stats.bands, a bit per 8-row strip. */
    CHECK(ksn_render_band_count(0)==0&&ksn_render_band_runs(0)==0);
    CHECK(ksn_render_band_count(1u)==1&&ksn_render_band_runs(1u)==1);
    CHECK(ksn_render_band_count(0x1fu)==5&&ksn_render_band_runs(0x1fu)==1);
    CHECK(ksn_render_band_count(0x10001u)==2&&ksn_render_band_runs(0x10001u)==2);
    CHECK(ksn_render_band_count(0x1ffffu)==17&&ksn_render_band_runs(0x1ffffu)==1);
    CHECK(ksn_render_band_count(0x55u)==4&&ksn_render_band_runs(0x55u)==4);
    puts("render prof PASS: arms byte-identical, counts move only with the switch on");
#else
    uint32_t off=0;
    CHECK(render_once(0,&off)==0);
    printf("RENDER_PROF pixels_off=0x%08x (no counters in this tree)\n",off);
#endif
    return 0;
}
