"""Design 0 (BOARD_STRIP_HALF, docs/platform/lcd-dma-driver.md 9.5): the actual
board functions with the strip's two 4-row halves taking turns.

The fake driver snapshots a payload when it is queued and checks, when it is
reaped and whenever the test writes pixels, that nothing touched it while it
was in flight -- the one thing design 0 must never do, since it has no
transmit copy. Pixels go out LSB first (no swap) and land on a model glass.

Cases: a Kasane-style frame of 4-row pieces, full width and narrow (the lent
half is packed in place); a screen's own whole strip, full width and narrow,
which must be finished on return and, narrow, left untouched; and the two
mixed across a frame end. Run again with BOARD_LCD_DIRECT (lcd_spi.c's
register-driven sends, no per-strip ISR): the same cases, every async piece
started and reaped directly.
"""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'main/hal/board.c').read_text()


def function(signature):
    start = source.index(signature)
    begin = source.index('{', start)
    depth = 0
    for token in re.finditer(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|[{}]',
                             source[begin:], re.S):
        if token.group() == '{':
            depth += 1
        elif token.group() == '}':
            depth -= 1
            if depth == 0:
                return source[start:begin + token.end()]
    raise ValueError('unterminated production function: ' + signature)


DECLARATIONS = '\n'.join(re.findall(
    r'^static (?:uint16_t|int|bool|spi_transaction_t) '
    r'(?:shared|tx_inflight|tx_pending|next_row|window_x|window_cols|\*half_lent|g_board_direct|tx_direct)\b[^;]*;',
    source, re.M))
assert 'half_lent' in DECLARATIONS, 'board.c has no design-0 state'

HEADER = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 22
#define ESP_ERR_TIMEOUT 23
#define ESP_ERR_INVALID_STATE 24
#define LCD_W 240
#define LCD_H 135
#define STRIP_H 8
#define portMAX_DELAY 0
#define BOARD_STRIP_HALF 1
#define DD_SCOPE(x) ((void)0)
#define DD_OPEN(x,y) ((void)0)
#define DD_CLOSE(x) ((void)0)
#define DD_ADD(x,y) ((void)(y))
#define LCD_TX_COUNT(e) ((void)(e))
#define gpio_set_level(p,v) ((void)(p),(void)(v))
#define pdMS_TO_TICKS(x) (x)
#define vTaskDelay(x) ((void)(x))
typedef struct {size_t length; const void *tx_buffer;} spi_transaction_t;
@DECLARATIONS@
static bool capture,capture_turn_taken,pie_swap,lcd_le=true;
static int g_board_async=1,g_board_swap_into=1,lcd;
static uint16_t glass[LCD_W*LCD_H],expected[LCD_W*LCD_H];
static uint8_t snapshot[LCD_W*STRIP_H*2];
static const uint8_t *borrowed;
static size_t borrow_bytes;
static bool bus_pending,ramwr,pending_direct;
static unsigned queues,polled_rows;
static int last_cmd,x0,x1,y0,y1,px,py;
static void check_borrow(void){
 if(bus_pending)assert(memcmp(borrowed,snapshot,borrow_bytes)==0);
}
static void pixels(const void *data,size_t size){
 assert(ramwr&&size%2==0);
 const uint8_t *bytes=data;
 for(size_t i=0;i<size;i+=2){
  assert(px>=x0&&px<=x1&&py>=y0&&py<=y1);
  glass[py*LCD_W+px]=(uint16_t)(bytes[i]|(bytes[i+1]<<8));   /* LSB first */
  if(++px>x1){px=x0;if(++py>y1)py=y0;}
 }
}
static esp_err_t tx(bool data,const void *bytes,size_t n){
 assert(!bus_pending);
 if(!data){
  assert(n==1);last_cmd=*(const uint8_t*)bytes;
  if(last_cmd==0x2c){px=x0;py=y0;ramwr=true;}else ramwr=false;
 }else if(last_cmd==0x2a&&n==4){
  const uint8_t *p=bytes;x0=((p[0]<<8)|p[1])-40;x1=((p[2]<<8)|p[3])-40;last_cmd=0;
 }else if(last_cmd==0x2b&&n==4){
  const uint8_t *p=bytes;y0=((p[0]<<8)|p[1])-53;y1=((p[2]<<8)|p[3])-53;last_cmd=0;
 }else{polled_rows++;pixels(bytes,n);}
 return ESP_OK;
}
static esp_err_t spi_device_queue_trans(int device,spi_transaction_t *t,int wait){
 (void)device;(void)wait;assert(!bus_pending);queues++;
 borrowed=t->tx_buffer;borrow_bytes=t->length/8;assert(borrow_bytes<=sizeof(snapshot));
 memcpy(snapshot,borrowed,borrow_bytes);bus_pending=true;return ESP_OK;
}
static esp_err_t spi_device_get_trans_result(int device,spi_transaction_t **done,int wait){
 (void)device;(void)wait;assert(bus_pending&&!pending_direct);check_borrow();
 *done=&tx_pending;bus_pending=false;pixels(borrowed,borrow_bytes);return ESP_OK;
}
#ifndef BOARD_LCD_DIRECT
#define BOARD_LCD_DIRECT 0
#endif
#if BOARD_LCD_DIRECT
/* lcd_spi.c: the strip starts on GP-SPI2/GDMA directly and is reaped by its
 * done bit, with no queue or ISR; same ownership rules as the queue. */
static unsigned direct_starts;
static void lcd_spi_direct_start(const void *data,size_t bytes){
 assert(!bus_pending);queues++;direct_starts++;
 borrowed=data;borrow_bytes=bytes;assert(borrow_bytes<=sizeof(snapshot));
 memcpy(snapshot,borrowed,borrow_bytes);bus_pending=pending_direct=true;
}
static esp_err_t lcd_spi_direct_wait(void){
 /* The done bit is polled only for a strip started directly. */
 if(!bus_pending)return ESP_OK;
 assert(pending_direct);check_borrow();bus_pending=pending_direct=false;pixels(borrowed,borrow_bytes);return ESP_OK;
}
#endif
static void pet_hub_overlay(uint16_t *p,int y,int rows){(void)p;(void)y;(void)rows;}
static void pocket_capture_overlay(uint16_t *p,int y,int rows){(void)p;(void)y;(void)rows;}
static void swap_scalar(uint16_t *out,const uint16_t *in,int n){(void)out;(void)in;(void)n;assert(0);}
static void swap_pie(const uint16_t *in,uint16_t *out,unsigned n){(void)in;(void)out;(void)n;assert(0);}
'''

CASES = r'''
static uint16_t value(int x,int y,int salt){return (uint16_t)(x*7+y*251+salt*977+1);}
/* The test writes a strip like a renderer: every pixel, checking afterwards
 * that the payload in flight was not one of them. */
static void draw(uint16_t *p,int y,int rows,int salt){
 for(int r=0;r<rows;r++)for(int x=0;x<LCD_W;x++)p[r*LCD_W+x]=value(x,y+r,salt);
 check_borrow();
}
static void expect(int x,int y,int cols,int rows,int salt){
 for(int r=0;r<rows;r++)for(int c=0;c<cols;c++)expected[(y+r)*LCD_W+x+c]=value(x+c,y+r,salt);
}
static void kasane_frame(int x,int cols,int salt){
 for(int y=0;y<LCD_H;y+=4){
  int rows=LCD_H-y<4?LCD_H-y:4;
  uint16_t *p=board_strip_next();
  assert(p==shared||p==shared+4*LCD_W);
  draw(p,y,rows,salt);
  expect(x,y,cols,rows,salt);
  assert(board_present_submit(x,y,cols,rows,p)==ESP_OK);
  assert(bus_pending);                    /* a lent half goes out async */
 }
 assert(board_present_finish()==ESP_OK&&!bus_pending);
 board_present_frame_end();
}
int main(void){
 assert(board_strip_rows()==4);
 /* Kasane, full width then narrow. */
 kasane_frame(0,LCD_W,1);
 assert(memcmp(glass,expected,sizeof glass)==0);
 kasane_frame(32,48,2);
 assert(memcmp(glass,expected,sizeof glass)==0);
 /* A screen's own whole strip, full width: finished on return. */
 for(int y=0;y<LCD_H;y+=STRIP_H){
  int rows=LCD_H-y<STRIP_H?LCD_H-y:STRIP_H;
  draw(shared,y,rows,3);expect(0,y,LCD_W,rows,3);
  assert(board_present(y,rows,shared)==ESP_OK);
  assert(!bus_pending);
 }
 assert(memcmp(glass,expected,sizeof glass)==0);
 /* Narrow: finished on return, and the caller's strip left as it was. */
 static uint16_t before[LCD_W*STRIP_H];
 unsigned polled=polled_rows;
 for(int y=0;y<LCD_H;y+=STRIP_H){
  int rows=LCD_H-y<STRIP_H?LCD_H-y:STRIP_H;
  draw(shared,y,rows,4);expect(40,y,64,rows,4);
  memcpy(before,shared,sizeof before);
  assert(board_present_rect(40,y,64,rows,shared)==ESP_OK);
  assert(!bus_pending&&memcmp(before,shared,sizeof before)==0);
 }
 assert(polled_rows-polled==LCD_H);       /* row by row */
 assert(memcmp(glass,expected,sizeof glass)==0);
 /* Mixed across frame ends. */
 for(int k=0;k<3;k++){
  kasane_frame(k*16,LCD_W-k*32,10+k);
  draw(shared,0,STRIP_H,20+k);expect(0,0,LCD_W,STRIP_H,20+k);
  assert(board_present(0,STRIP_H,shared)==ESP_OK&&!bus_pending);
  board_present_frame_end();
 }
 assert(memcmp(glass,expected,sizeof glass)==0);
 #if BOARD_LCD_DIRECT
 assert(direct_starts==queues);           /* every async piece went direct */
#endif
 printf("BOARD_HALF PASS%s: %u queued pieces, %u polled rows; no write into a payload in flight\n",
        BOARD_LCD_DIRECT?" (direct sends)":"",queues,polled_rows);
 return 0;
}
'''

parts = [function(x) for x in (
    'static void put_strip(uint16_t *out,uint16_t *in,int count){', 'int board_strip_rows(',
    'uint16_t *board_strip_next(', 'static esp_err_t tx_reap(', 'void board_capture(',
    'bool board_capture_take_turn(', 'static esp_err_t command(', 'esp_err_t board_present(',
    'esp_err_t board_present_rect(', 'void board_present_frame_end(',
    'esp_err_t board_present_finish(', 'esp_err_t board_present_submit(')]
with tempfile.TemporaryDirectory(prefix='board-half-') as tmp:
    path = Path(tmp) / 'test.c'
    path.write_text(HEADER.replace('@DECLARATIONS@', DECLARATIONS) + '\n'.join(parts) + CASES)
    for flags in (['-O2', '-fstrict-aliasing'], ['-g', '-fsanitize=address,undefined'],
                  ['-g', '-fsanitize=address,undefined', '-DBOARD_LCD_DIRECT=1'],
                  ['-O2', '-fstrict-aliasing', '-DBOARD_LCD_DIRECT=1']):
        exe = Path(tmp) / 'test'
        subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-variable', '-Wno-unused-function', '-DKASANE_DMA_OVERLAP=1',
                        *flags, str(path), '-o', str(exe)], check=True)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if result.returncode:
            raise SystemExit(result.stdout + result.stderr)
        print(result.stdout.strip())
