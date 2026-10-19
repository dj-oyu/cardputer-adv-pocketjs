"""Real owner-task board boundaries with a memory-only SPI model; no USB/device."""
from pathlib import Path
import subprocess,tempfile,sys
R=Path(__file__).resolve().parents[1];source=(R/'main/hal/board.c').read_text()
def function(sig):
    a=source.index(sig);b=source.index('\n}',a)+2;return source[a:b]+'\n'
head=r'''
#include "derby_detail.h"
#include "derby_measure.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 7
#define LCD_W 240
#define LCD_H 135
#define STRIP_H 8
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(x) (x)
typedef struct {size_t length;const void *tx_buffer;} spi_transaction_t;
static bool tx_inflight,capture,g_board_async=true,g_board_swap_into=true,pie_swap;
static int lcd,next_row=-1,tx_front,fail_queue,fail_reap;
static uint16_t storage[2][240*8],*tx_buf[2]={storage[0],storage[1]};
static spi_transaction_t tx_pending;
static uint64_t now;
uint64_t dm_now_us(void){return now;}
static void vTaskDelay(int t){(void)t;}
static void pet_hub_overlay(uint16_t *p,int y,int n){(void)p;(void)y;(void)n;}
static void pocket_capture_overlay(uint16_t *p,int y,int n){(void)p;(void)y;(void)n;}
static void swap_scalar(uint16_t *out,const uint16_t *in,int n){for(int i=0;i<n;i++)out[i]=(uint16_t)(in[i]<<8|in[i]>>8);now+=2;}
static void swap_pie(const uint16_t *in,uint16_t *out,unsigned n){swap_scalar(out,in,(int)(n*16));}
static void gpio_set_level(int p,int v){(void)p;(void)v;}
static int spi_device_queue_trans(int d,spi_transaction_t *t,int timeout){(void)d;(void)t;(void)timeout;now+=3;return fail_queue;}
static int spi_device_get_trans_result(int d,spi_transaction_t **t,int timeout){(void)d;(void)timeout;*t=&tx_pending;now+=11;return fail_reap;}
static int command(unsigned c,const void *p,size_t n){(void)c;(void)p;(void)n;return 0;}
static int tx(bool data,const void *p,size_t n){(void)data;(void)p;(void)n;now+=17;return 0;}
'''
body=''.join(function(s) for s in ('static esp_err_t tx_reap(void)', 'esp_err_t board_present(int ', 'esp_err_t board_present_rect(int ', 'esp_err_t board_present_rect_sync(int ', 'esp_err_t board_present_sync(int '))
cases=r'''
int main(void){
 uint16_t pixels[240*8]={0};dd_snapshot s;
 dd_start();assert(board_present_sync(0,8,pixels)==0);assert(dd_finish(&s));
 assert(s.event[DD_TX_PAYLOAD_BYTES]==3840&&s.event[DD_TX_COPY_BYTES]==3840);
 assert(s.event[DD_TX_SUBMITS]==1&&s.event[DD_TX_REAPS]==1&&!s.faults);
 assert(s.stage[DD_TX_SUBMIT].inclusive_us==3&&s.stage[DD_TX_REAP].inclusive_us==11);
 dd_start();assert(board_present_rect_sync(16,8,160,7,pixels)==0);assert(dd_finish(&s));
 assert(s.event[DD_TX_PAYLOAD_BYTES]==2240&&s.event[DD_TX_COPY_BYTES]==2240);
 dd_start();fail_queue=9;assert(board_present_sync(0,8,pixels)==9);assert(dd_finish(&s));
 assert(s.event[DD_TX_FAILURES]==1&&!s.event[DD_TX_PAYLOAD_BYTES]&&!s.event[DD_TX_REAPS]);
 dd_start();fail_queue=0;fail_reap=8;assert(board_present_sync(0,8,pixels)==8);assert(dd_finish(&s));
 assert(s.event[DD_TX_FAILURES]==1&&s.event[DD_TX_REAPS]==1);
 dd_start();fail_reap=0;g_board_async=false;assert(board_present_sync(0,8,pixels)==0);assert(dd_finish(&s));
 assert(s.event[DD_TX_PAYLOAD_BYTES]==3840&&s.event[DD_TX_SUBMITS]==1&&!s.event[DD_TX_REAPS]);
 assert(s.stage[DD_TX_SUBMIT].inclusive_us==17&&!s.faults);
 puts("PASS actual full/narrow async+blocking board paths; queue/reap failures, logical bytes, elapsed boundaries (SPI stub)");
}
'''
with tempfile.TemporaryDirectory() as tmp:
 p=Path(tmp);(p/'test.c').write_text(head+body+cases)
 subprocess.run(['gcc','-std=gnu11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-DPOCKET_DERBY_MEASURE','-DPOCKET_DERBY_DETAIL_MEASURE','-I',str(R/'main/pocket'),str(p/'test.c'),str(R/'main/pocket/derby_measure.c'),'-o',str(p/'test')],check=True)
 subprocess.run([p/'test'],check=True)
