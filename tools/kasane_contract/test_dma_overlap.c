#include "core_fixture.h"
#include "ksn_render.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
typedef struct {
 uint16_t panel[240*135], strip[240*8], tx[2][240*8];
 unsigned x,y,cols,rows,front,queues,overlaps;
 int pending,delayed,fail_y,fail_finish;
 unsigned finishes;ksn_core *core;
} output;
static int complete(output *o){
 if(!o->pending)return 0;
 for(unsigned r=0;r<o->rows;r++)memcpy(o->panel+(o->y+r)*240+o->x,o->tx[o->front]+r*o->cols,o->cols*2);
 o->pending=0;return 0;
}
static uint16_t *strip(void *ctx){output *o=ctx;return o->strip;}
static ksn_result send_rect(void *ctx,uint16_t x,uint16_t y,uint16_t cols,uint16_t rows,const uint16_t *p){
 output *o=ctx;if(o->pending)o->overlaps++;complete(o);
 if(y==o->fail_y)return KSN_IO;
 o->front^=1;o->x=x;o->y=y;o->cols=cols;o->rows=rows;
 for(unsigned r=0;r<rows;r++)memcpy(o->tx[o->front]+r*cols,p+r*240+x,cols*2);
 o->pending=1;o->queues++;if(!o->delayed)complete(o);return KSN_OK;
}
static ksn_result send(void *ctx,uint16_t y,uint16_t rows,const uint16_t *p){return send_rect(ctx,0,y,240,rows,p);}
static void scene(ksn_core *c,unsigned i){
 ksn_client client=ksn_core_client(c,KSN_APP);ksn_tx t;ksn_ref ref;
 assert(client.ops->begin(client.ctx,KSN_REPLACE,&t)==KSN_OK);
 assert(client.ops->background(client.ctx,t,0x123456ff+i*256)==KSN_OK);
 ksn_draw d={.kind=KSN_RECT,.bounds={(int16_t)(i%70),7,180,129},.clip={0,0,240,135},.opacity=255,.data.shape.color=0xabcdefa0};
 assert(client.ops->add(client.ctx,t,&d,&ref)==KSN_OK);
 assert(client.ops->end(client.ctx,t)==KSN_OK);
}
static ksn_result finish(void *ctx){
 output *o=ctx;o->finishes++;
 if(o->pending&&ksn_core_has_submission(o->core))
  assert(ksn_core_poll(o->core).status==KSN_SUBMITTED);
 complete(o);
 if(o->fail_finish){o->fail_finish=0;return KSN_IO;}return KSN_OK;
}
static ksn_result backdrop(void *ctx,uint16_t y,uint16_t rows,uint16_t *p){
 output *o=ctx;if(y==16)return KSN_IO;
 if(o->pending)o->overlaps++;
 for(unsigned i=0;i<rows*240u;i++)p[i]=0x1234;
 return KSN_OK;
}
int main(void){
 KSN_TEST_CORE(sync,static);KSN_TEST_CORE(async,static);
 ksn_core_init(&sync);ksn_core_init(&async);
 static output a={.fail_y=-1},b={.delayed=1,.fail_y=-1};
 ksn_display_port ap={.ctx=&a,.strip=strip,.present=send,.present_rect=send_rect,.width=240,.height=135,.strip_rows=8};
 ksn_display_port bp=ap;bp.ctx=&b;
 a.core=&sync;b.core=&async;
 ksn_display_completion completion={&b,finish};
 ksn_render_stats ast,bst;
 for(unsigned frame=0;frame<100;frame++){
  scene(&sync,frame);scene(&async,frame);
  assert(ksn_render_rects(&sync,&ap,&ast)==KSN_OK);
  assert(ksn_render_rects_complete(&async,&bp,NULL,false,&completion,&bst)==KSN_OK);
  assert(ast.transferred_bytes==64800&&bst.transferred_bytes==64800);
  assert(!b.pending&&ksn_core_poll(&async).status==KSN_PRESENTED);
  /* The final seven rows complete before the bank is acknowledged. */
  assert(b.y==128&&b.rows==7);complete(&b);
  assert(memcmp(a.panel,b.panel,sizeof(a.panel))==0);
 }
 assert(b.overlaps>=1600);
 /* Alternate narrow, full-width and disjoint damage, including the last band. */
 for(unsigned i=0;i<60;i++){
  ksn_rect damage={(int16_t)(i%2?0:17),(int16_t)(i%3?8:128),
                   (int16_t)(i%2?240:34),(int16_t)(i%3?16:135)};
  ksn_core_invalidate_rect(&sync,damage);ksn_core_invalidate_rect(&async,damage);
  assert(ksn_render_rects(&sync,&ap,&ast)==KSN_OK);
  assert(ksn_render_rects_complete(&async,&bp,NULL,false,&completion,&bst)==KSN_OK);
  assert(!b.pending&&ast.transferred_bytes==bst.transferred_bytes);
  assert(memcmp(a.panel,b.panel,sizeof(a.panel))==0);
 }
 /* A failed final completion cannot promote the candidate, and retry drains. */
 scene(&async,101);b.fail_finish=1;
 assert(ksn_render_rects_complete(&async,&bp,NULL,false,&completion,&bst)==KSN_IO);
 assert(!b.pending&&ksn_core_poll(&async).status==KSN_SUBMITTED);
 assert(ksn_render_rects_complete(&async,&bp,NULL,false,&completion,&bst)==KSN_OK);
 assert(!b.pending&&ksn_core_poll(&async).status==KSN_PRESENTED);
 /* Provider failure after earlier queued bands still completes before return. */
 scene(&async,102);
 assert(ksn_render_rects_complete(&async,&bp,backdrop,false,&completion,&bst)==KSN_IO);
 assert(!b.pending&&ksn_core_poll(&async).status==KSN_SUBMITTED);
 /* Cancellation/teardown after failure has no surviving pixel borrow. */
 assert(ksn_core_discard(&async,ksn_core_poll(&async).ticket)==KSN_OK);
 assert(ksn_core_reset_layer(&async,KSN_APP)==KSN_OK&&!b.pending);
 scene(&async,103);b.fail_y=8;
 assert(ksn_render_rects_complete(&async,&bp,NULL,false,&completion,&bst)==KSN_IO);
 assert(!b.pending&&ksn_core_poll(&async).status==KSN_SUBMITTED);b.fail_y=-1;
 assert(ksn_render_rects_complete(&async,&bp,NULL,false,&completion,&bst)==KSN_OK);
 /* Zero damage still passes through completion before ACK. */
 ksn_client client=ksn_core_client(&async,KSN_APP);ksn_tx t;
 assert(client.ops->begin(client.ctx,KSN_PATCH,&t)==KSN_OK);
 assert(client.ops->end(client.ctx,t)==KSN_OK);
 unsigned before=b.finishes;
 assert(ksn_render_rects_complete(&async,&bp,NULL,false,&completion,&bst)==KSN_OK);
 assert(b.finishes==before+1&&bst.transferred_bytes==0&&!b.pending);
 /* Invalid calls cannot accidentally ACK an unprepared ticket. */
 before=b.finishes;
 assert(ksn_render_rects_complete(NULL,&bp,NULL,false,&completion,&bst)==KSN_INVALID);
 assert(b.finishes==before+1);
 puts("DMA completion renderer: 100 pixel-oracle frames + 60 narrow/full damage; ACK, final 7 rows, provider/send/finish failures, retry/cancel/reset/no-damage: PASS");
}
