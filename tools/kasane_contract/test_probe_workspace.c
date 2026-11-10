/* Whole diagnostic on a synthetic copied display, real core/cache/font/frost.
 * Virtual time, heap counters and HAL are fakes: this cannot measure speed. */
#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "board.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "../../main/ui/diagnostic_session.h"
#include "../../main/text/jpfont.c"
static union {uint32_t align;uint8_t bytes[512];} test_font_image;
static uint16_t host_strip[240*8],host_tx[240*8];
static unsigned queued_y,queued_rows,send_count,finish_count,restore_count,release_count;
static bool pending,view_seen,fail_transition,transition_failed;
static uint64_t digest,virtual_us;
static FILE *pixel_trace,*message_trace;
static void font_setup(void){
 jpf_hdr_t h={.magic=JPF_MAGIC,.version=1,.cell_w=12,.cell_h=12,.count=3,.cmap_off=sizeof(jpf_hdr_t),.bitmap_off=64,.bitmap_len=72};
 jpf_cmap_t cmap[3]={{0,12,0},{'A',6,0},{0x3042,12,0}};
 memcpy(test_font_image.bytes,&h,sizeof h);memcpy(test_font_image.bytes+h.cmap_off,cmap,sizeof cmap);
 for(unsigned g=0;g<3;g++)for(unsigned y=0;y<12;y++){
  test_font_image.bytes[64+g*24+y*2]=(uint8_t)(0x80u>>(y%6));
  test_font_image.bytes[65+g*24+y*2]=(uint8_t)(g==2?0xa0:0);
 }
 assert(open_face(&faces[JPFONT_TEXT],test_font_image.bytes,sizeof test_font_image.bytes));
}
int64_t esp_timer_get_time(void){virtual_us+=100;return (int64_t)virtual_us;}
void vTaskDelay(unsigned ticks){virtual_us+=(uint64_t)ticks*1000;}
unsigned uxTaskGetStackHighWaterMark(void *task){(void)task;return 0;}
size_t heap_caps_get_free_size(unsigned caps){(void)caps;return 1000000;}
size_t heap_caps_get_largest_free_block(unsigned caps){(void)caps;return 1000000;}
void test_probe_log(const char *level,const char *tag,const char *fmt,...){
 if(message_trace){
  fprintf(message_trace,"%s (%s) ",level,tag);
  va_list ap;va_start(ap,fmt);vfprintf(message_trace,fmt,ap);va_end(ap);
  fputc('\n',message_trace);
 }
 if(strncmp(fmt,"VIEW PASS",9)==0)view_seen=true;
}
uint16_t *board_strip(void){return host_strip;}
uint16_t board_rgb(unsigned r,unsigned g,unsigned b){return (uint16_t)(((r&248)<<8)|((g&252)<<3)|(b>>3));}
void board_capture(bool on){(void)on;}
static void hash_word(unsigned value){digest=(digest^value)*UINT64_C(1099511628211);}
/* The transition injection is handled after the real source is included. */
static void capture_transition(void);
esp_err_t board_present_finish(void){
 ++finish_count;
 if(fail_transition&&view_seen&&!transition_failed){
  capture_transition();transition_failed=true;return ESP_FAIL;
 }
 if(pending){
  if(pixel_trace){
   uint32_t header[2]={queued_y,queued_rows};
   assert(fwrite(header,sizeof header,1,pixel_trace)==1);
   assert(fwrite(host_tx,sizeof *host_tx,queued_rows*240,pixel_trace)==queued_rows*240);
  }
  hash_word(queued_y);hash_word(queued_rows);
  for(unsigned i=0;i<queued_rows*240;i++)hash_word(host_tx[i]);
  pending=false;
 }
 return ESP_OK;
}
esp_err_t board_present(int y,int rows,uint16_t *pixels){
 assert(pixels==host_strip&&y>=0&&rows>0&&rows<=8&&y+rows<=135);
 if(board_present_finish()!=ESP_OK)return ESP_FAIL;
 memcpy(host_tx,pixels,(size_t)rows*240*sizeof *pixels);
 queued_y=(unsigned)y;queued_rows=(unsigned)rows;pending=true;send_count++;return ESP_OK;
}
esp_err_t board_present_sync(int y,int rows,uint16_t *pixels){
 if(board_present_finish()!=ESP_OK)return ESP_FAIL;
 if(board_present(y,rows,pixels)!=ESP_OK)return ESP_FAIL;
 return board_present_finish();
}
#include PROBE_SOURCE
#ifdef TEST_SHARED_WORKSPACE
static unsigned char transition_bytes[sizeof probe_workspace_storage];
static void capture_transition(void){memcpy(transition_bytes,&probe_workspace_storage,sizeof transition_bytes);}
#else
static void capture_transition(void){}
#endif
static unsigned owner_token;
static void *acquire(void *ctx){(void)ctx;return &owner_token;}
static bool drain(void *ctx){(void)ctx;return board_present_finish()==ESP_OK;}
static bool work(void *ctx,void *owner){(void)ctx;assert(owner==&owner_token);return ksn_device_probe_run()&&board_present_finish()==ESP_OK;}
static bool restore(void *ctx){(void)ctx;assert(!pending);restore_count++;return true;}
static void release(void *ctx,void *owner){(void)ctx;assert(owner==&owner_token&&!pending);release_count++;}
static const diagnostic_lifecycle_ops ops={.acquire=acquire,.drain=drain,.work=work,.restore=restore,.release=release};
int main(void){
 pixel_trace=fopen(getenv("PROBE_PIXEL_TRACE"),"wb");
 message_trace=fopen(getenv("PROBE_MESSAGE_TRACE"),"wb");
 assert(pixel_trace&&message_trace);
 font_setup();uint64_t first=0;unsigned first_sends=0;
 diagnostic_session session=DIAGNOSTIC_SESSION_INIT;
 for(unsigned run=0;run<2;run++){
  digest=UINT64_C(1469598103934665603);send_count=0;view_seen=false;
  assert(diagnostic_command_request(&session.command)==DIAG_REQUEST_QUEUED);
  assert(diagnostic_session_pump(&session,&ops,true)==DIAG_SESSION_COMPLETE);
  assert(!pending&&view_seen&&restore_count==run+1&&release_count==run+1);
  if(run){assert(digest==first&&send_count==first_sends);}else{first=digest;first_sends=send_count;}
  printf("HOST_PROBE run=%u sends=%u digest=%016llx\n",run,send_count,(unsigned long long)digest);
 }
 assert(!fclose(pixel_trace)&&!fclose(message_trace));pixel_trace=message_trace=NULL;
#ifdef TEST_SHARED_WORKSPACE
 assert(sizeof(probe_workspace_storage)==sizeof(probe_workspace_storage.cached));
 assert(sizeof(probe_workspace_storage.cached)>sizeof(ksn_frost));
 /* Real diagnostic reaches VIEW, then the transition fails before frost writes.
  * Its outer session must retain ownership and forbid any restart/callback. */
 view_seen=false;fail_transition=true;
 assert(diagnostic_command_request(&session.command)==DIAG_REQUEST_QUEUED);
 assert(diagnostic_session_pump(&session,&ops,true)==DIAG_SESSION_TERMINAL);
 assert(transition_failed&&session.lifecycle.quarantined&&session.lifecycle.owner==&owner_token);
 assert(!memcmp(transition_bytes,&probe_workspace_storage,sizeof transition_bytes));
 assert(restore_count==2&&release_count==2);
 unsigned before=send_count,finishes=finish_count;
 assert(diagnostic_command_request(&session.command)==DIAG_REQUEST_TERMINAL);
 assert(diagnostic_command_cancel(&session.command)==DIAG_CANCEL_TERMINAL);
 assert(diagnostic_session_pump(&session,&ops,true)==DIAG_SESSION_TERMINAL);
 assert(send_count==before&&finish_count==finishes);
 printf("HOST_WORKSPACE bytes=%zu saved=%zu transition_failure_no_write quarantine_no_restart PASS\n",sizeof(probe_workspace_storage),sizeof(ksn_frost));
#endif
 return 0;
}
