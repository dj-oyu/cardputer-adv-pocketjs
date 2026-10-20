"""Fault-inject the actual copied DMA boundary; no firmware or hardware imports."""
from pathlib import Path
import os
import subprocess
import tempfile
root=Path(__file__).resolve().parents[1]
source=(root/'main/hal/board.c').read_text()
def function(signature):
    start=source.index(signature)
    return source[start:source.index('\n}',start)+2]
HARNESS = r'''#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define DD_SCOPE(stage) ((void)0)
#define DD_OPEN(name,stage) ((void)0)
#define DD_CLOSE(name) ((void)0)
enum {DD_TX_FAILURES,DD_TX_REAPS,DD_TX_COPY_BYTES,DD_TX_SUBMITS,DD_TX_PAYLOAD_BYTES,DD_EVENTS};
static unsigned detail[DD_EVENTS];
#define DD_ADD(event,amount) (detail[event]+=(unsigned)(amount))
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 22
#define ESP_ERR_TIMEOUT 23
#define ESP_ERR_INVALID_STATE 24
#define LCD_W 240
#define LCD_H 135
#define STRIP_H 8
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(x) (x)
#define vTaskDelay(x) ((void)(x))
#define gpio_set_level(x,y) ((void)(x),(void)(y))
typedef struct {size_t length; const void *tx_buffer;} spi_transaction_t;
static uint16_t tx_buf[2][240*8], shared[240*8], saved[240*8];
static int tx_front, g_board_async=1, g_board_swap_into=1;
static bool tx_inflight, capture, pie_swap;
static spi_transaction_t tx_pending;
static int lcd, queues, reaps, command_calls, fail_queue;
static const void *borrow; static size_t borrowed_bytes;
static void pet_hub_overlay(uint16_t *p,int y,int rows){(void)p;(void)y;(void)rows;}
static void pocket_capture_overlay(uint16_t *p,int y,int rows){(void)p;(void)y;(void)rows;}
static void swap_scalar(uint16_t *out,const uint16_t *in,int n){
 for(int i=0;i<n;i++)out[i]=(uint16_t)((in[i]>>8)|(in[i]<<8));
}
static void swap_pie(const uint16_t *in,uint16_t *out,unsigned n){swap_scalar(out,in,(int)n*16);}
static esp_err_t tx_reap(void);
static int next_row=-1;
static esp_err_t command(uint8_t,const void *,size_t);
static int timeout_once, unknown_once, dma_fail_once, descriptor_once;
static spi_transaction_t foreign_descriptor;
static esp_err_t spi_device_get_trans_result(int device,spi_transaction_t **done,int wait){
 (void)device;(void)wait;assert(tx_inflight);
 assert(borrow==tx_pending.tx_buffer);
 assert(memcmp(borrow,saved,borrowed_bytes)==0);
 if(timeout_once){timeout_once=0;return ESP_ERR_TIMEOUT;}
 if(unknown_once){unknown_once=0;return ESP_ERR_INVALID_ARG;}
 if(descriptor_once){
  int mode=descriptor_once;descriptor_once=0;
  *done=mode==1?NULL:&foreign_descriptor;
  return mode==3?ESP_ERR_INVALID_ARG:ESP_OK;
 }
 *done=&tx_pending;reaps++;
 if(dma_fail_once){dma_fail_once=0;return ESP_ERR_INVALID_STATE;}
 return ESP_OK;
}
static esp_err_t tx(bool d,const void *p,size_t n){(void)d;(void)p;(void)n;assert(!tx_inflight);command_calls++;return ESP_OK;}
static esp_err_t spi_device_queue_trans(int device,spi_transaction_t *t,int wait){
 (void)device;(void)wait;assert(!tx_inflight);queues++;
 assert(t->length/8<=sizeof(saved));
 if(fail_queue){fail_queue=0;return 32;}
 borrow=t->tx_buffer;borrowed_bytes=t->length/8;memcpy(saved,borrow,borrowed_bytes);return ESP_OK;
}
'''
CASES = r'''int main(void){
 for(int frame=0;frame<100;frame++){
  for(int y=0;y<135;y+=8){
   int rows=y==128?7:8;
   for(int i=0;i<240*rows;i++)shared[i]=(uint16_t)(frame*73+y*240+i);
   int x=frame%3==0?16:0, cols=x?32:240;
   assert(board_present_submit(x,y,cols,rows,shared)==ESP_OK);
   assert(tx_inflight);
   /* Rendering the next strip may overwrite every source pixel immediately. */
   memset(shared,0xa5,sizeof(shared));
  }
  assert(board_present_finish()==ESP_OK&&!tx_inflight);
 }
 assert(queues==1700&&reaps==1700);
 /* Prior completion errors must survive a re-window command. */
 assert(board_present_submit(0,0,240,8,shared)==ESP_OK);int c=command_calls,q=queues;
 dma_fail_once=1;assert(board_present_submit(16,8,32,8,shared)==ESP_ERR_INVALID_STATE);
 assert(command_calls==c&&queues==q&&next_row==-1&&!tx_inflight);
 fail_queue=1;assert(board_present_submit(0,0,240,8,shared)==32);
 assert(!tx_inflight&&next_row==-1&&board_present_finish()==ESP_OK);
 assert(board_present_submit(0,0,240,8,shared)==ESP_OK);dma_fail_once=1;
 assert(board_present_finish()==ESP_ERR_INVALID_STATE&&!tx_inflight&&next_row==-1);
 /* A timeout must retry without releasing payload or descriptor. */
 assert(board_present_submit(0,0,240,8,shared)==ESP_OK);timeout_once=1;
 assert(board_present_finish()==ESP_OK&&!tx_inflight);
 /* An unreturned descriptor quarantines ownership and cannot be overwritten. */
 assert(board_present_submit(0,0,240,8,shared)==ESP_OK);
 unknown_once=1;assert(board_present_finish()==ESP_ERR_INVALID_ARG&&tx_inflight);
 const void *old_borrow=borrow;spi_transaction_t old_descriptor=tx_pending;
 unknown_once=1;assert(board_present_submit(16,8,32,8,shared)==ESP_ERR_INVALID_ARG);
 assert(tx_inflight&&borrow==old_borrow&&memcmp(&old_descriptor,&tx_pending,sizeof(tx_pending))==0);
 assert(board_present_finish()==ESP_OK&&!tx_inflight);
 assert(board_present_submit(0,0,240,8,shared)==ESP_OK);
 unknown_once=1;c=command_calls;
 assert(command(0x2c,NULL,0)==ESP_ERR_INVALID_ARG&&tx_inflight);
 assert(command_calls==c&&board_present_finish()==ESP_OK);
 /* Missing/foreign descriptors count the normalized failure once per reap.
  * Quarantined DMA storage and all payload/copy/submit meanings survive. */
 for(int mode=1;mode<=3;mode++){
  assert(board_present_submit(0,0,240,8,shared)==ESP_OK);
  unsigned failures=detail[DD_TX_FAILURES],attempts=detail[DD_TX_REAPS];
  unsigned payload=detail[DD_TX_PAYLOAD_BYTES],copies=detail[DD_TX_COPY_BYTES];
  unsigned submits=detail[DD_TX_SUBMITS];c=command_calls;q=queues;
  old_borrow=borrow;old_descriptor=tx_pending;
  descriptor_once=mode;
  assert(board_present_finish()==(mode==3?ESP_ERR_INVALID_ARG:ESP_ERR_INVALID_STATE));
  assert(tx_inflight&&next_row==-1&&detail[DD_TX_FAILURES]==failures+1);
  assert(detail[DD_TX_REAPS]==attempts+1&&borrow==old_borrow);
  assert(memcmp(&old_descriptor,&tx_pending,sizeof(tx_pending))==0);
  descriptor_once=mode;
  assert(board_present_submit(16,8,32,8,shared)==(mode==3?ESP_ERR_INVALID_ARG:ESP_ERR_INVALID_STATE));
  assert(tx_inflight&&command_calls==c&&queues==q);
  assert(detail[DD_TX_FAILURES]==failures+2&&detail[DD_TX_REAPS]==attempts+2);
  assert(detail[DD_TX_PAYLOAD_BYTES]==payload&&detail[DD_TX_COPY_BYTES]==copies);
  assert(detail[DD_TX_SUBMITS]==submits);
  assert(board_present_finish()==ESP_OK&&!tx_inflight);
  assert(board_present_finish()==ESP_OK&&detail[DD_TX_FAILURES]==failures+2);
  assert(detail[DD_TX_REAPS]==attempts+3);
 }
 /* Cancellation/exit must drain even if no next submit occurs. */
 assert(board_present_submit(16,0,32,8,shared)==ESP_OK);
 assert(board_present_finish()==ESP_OK&&!tx_inflight);
 assert(board_present_finish()==ESP_OK);
 /* Reference synchronous path acknowledges only drained work. */
 assert(board_present_sync(0,8,shared)==ESP_OK&&!tx_inflight);
 assert(board_present_rect_sync(16,8,32,8,shared)==ESP_OK&&!tx_inflight);
 assert(board_present_submit(0,134,240,8,shared)==ESP_ERR_INVALID_ARG);
 assert(!tx_inflight&&board_present_finish()==ESP_OK);
 /* Existing polling and in-place-swap switches remain usable. */
 g_board_async=0;
 assert(board_present_submit(0,0,240,8,shared)==ESP_OK&&!tx_inflight);
 assert(board_present_submit(16,8,32,8,shared)==ESP_OK&&!tx_inflight);
 assert(board_present_finish()==ESP_OK);g_board_async=1;g_board_swap_into=0;
 assert(board_present_submit(0,0,240,8,shared)==ESP_OK&&tx_inflight);
 memset(shared,0x5a,sizeof(shared));assert(board_present_finish()==ESP_OK);
 assert(sizeof(tx_buf)==7680&&sizeof(shared)==3840);
 puts("actual board submit/finish: 1700 strips; lifetime, full/narrow, final 7 rows, prior/queue/final failure, cancel/exit, fixed TX 7680 B: PASS");
}'''

parts=[function(s) for s in ['static esp_err_t tx_reap(', 'static esp_err_t command(',
    'esp_err_t board_present(', 'esp_err_t board_present_rect(',
    'esp_err_t board_present_sync(', 'esp_err_t board_present_rect_sync(',
    'esp_err_t board_present_finish(', 'esp_err_t board_present_submit(']]
with tempfile.TemporaryDirectory(prefix='kasane-dma-board-') as temp:
    path=Path(temp);c=path/'test.c';c.write_text(HARNESS+'\n'.join(parts)+CASES)
    for flags in [['-O2','-fstrict-aliasing'],['-g','-fsanitize=address,undefined']]:
        exe=path/'test'
        subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',
                        '-DKASANE_DMA_OVERLAP=1',*flags,str(c),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
