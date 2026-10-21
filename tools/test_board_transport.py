"""Actual board functions with a panel/payload-ownership model, DMA OFF and ON.

This models SPI ordering and panel addressing, not hardware timing. Every DMA
payload stays borrowed until the exact descriptor is returned, including faults.
"""
from pathlib import Path
import json
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
    r'(?:shared|tx_buf|tx_front|tx_inflight|tx_pending|next_row|window_x|window_cols)\b[^;]*;',
    source, re.M))
CONTINUATION = 'void board_present_frame_end(' in source
EXPECTED_TX_BYTES = 3840
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
#define DD_SCOPE(x) ((void)0)
#define DD_OPEN(x,y) ((void)0)
#define DD_CLOSE(x) ((void)0)
#define DD_ADD(x,y) ((void)(y))
#define pdMS_TO_TICKS(x) (x)
#define vTaskDelay(x) ((void)(x))
#define gpio_set_level(p,v) ((void)(p),(void)(v))
typedef struct {size_t length; const void *tx_buffer;} spi_transaction_t;
@DECLARATIONS@
static bool capture,capture_turn_taken,pie_swap;
static int g_board_async=1,g_board_swap_into=1,lcd;
static uint16_t glass[LCD_W*LCD_H],expected[LCD_W*LCD_H];
static uint8_t snapshot[LCD_W*STRIP_H*2];
static const uint8_t *borrowed;
static size_t borrow_bytes,wire_bytes,copy_bytes;
static bool bus_pending,ramwr;
static unsigned commands,control_calls,queues,reaps,copies_while_pending;
static int last_cmd,x0,x1,y0,y1,px,py;
static int fault,queue_error,control_error_at;
static spi_transaction_t foreign;
static void check_borrow(void){
 if(bus_pending)assert(memcmp(borrowed,snapshot,borrow_bytes)==0);
}
static void pixels(const void *data,size_t size){
 assert(ramwr&&size%2==0);
 const uint8_t *bytes=data;
 for(size_t i=0;i<size;i+=2){
  assert(px>=x0&&px<=x1&&py>=y0&&py<=y1);
  assert(px>=0&&px<LCD_W&&py>=0&&py<LCD_H);
  glass[py*LCD_W+px]=(uint16_t)((bytes[i]<<8)|bytes[i+1]);
  if(++px>x1){px=x0;if(++py>y1)py=y0;}
 }
 wire_bytes+=size;
}
static esp_err_t tx(bool data,const void *bytes,size_t n){
 assert(!bus_pending);control_calls++;
 if(control_error_at==(int)control_calls)return 25;
 if(!data){
  assert(n==1);last_cmd=*(const uint8_t*)bytes;commands++;
  if(last_cmd==0x2c){px=x0;py=y0;ramwr=true;}
  else ramwr=false;
 }else if(last_cmd==0x2a&&n==4){
  const uint8_t *p=bytes;x0=((p[0]<<8)|p[1])-40;x1=((p[2]<<8)|p[3])-40;last_cmd=0;
 }else if(last_cmd==0x2b&&n==4){
  const uint8_t *p=bytes;y0=((p[0]<<8)|p[1])-53;y1=((p[2]<<8)|p[3])-53;last_cmd=0;
 }else pixels(bytes,n);
 return ESP_OK;
}
static esp_err_t spi_device_queue_trans(int device,spi_transaction_t *t,int wait){
 (void)device;(void)wait;assert(!bus_pending);queues++;
 if(queue_error){queue_error=0;return 31;}
 borrowed=t->tx_buffer;borrow_bytes=t->length/8;assert(borrow_bytes<=sizeof(snapshot));
 memcpy(snapshot,borrowed,borrow_bytes);bus_pending=true;return ESP_OK;
}
static esp_err_t spi_device_get_trans_result(int device,spi_transaction_t **done,int wait){
 (void)device;(void)wait;assert(bus_pending);check_borrow();reaps++;
 int f=fault;fault=0;
 if(f==1)return ESP_ERR_TIMEOUT;
 if(f==2)return ESP_ERR_INVALID_ARG;
 if(f==3){*done=&foreign;return ESP_OK;}
 *done=&tx_pending;bus_pending=false;
 if(f==4)return ESP_ERR_INVALID_STATE;
 pixels(borrowed,borrow_bytes);return ESP_OK;
}
static void pet_hub_overlay(uint16_t *p,int y,int rows){(void)p;(void)y;(void)rows;}
static void pocket_capture_overlay(uint16_t *p,int y,int rows){(void)p;(void)y;(void)rows;}
static void swap_scalar(uint16_t *out,const uint16_t *in,int n){
 check_borrow();
 if(bus_pending){
  copies_while_pending++;
  uintptr_t a=(uintptr_t)out,b=(uintptr_t)borrowed;
  assert(a+(size_t)n*2<=b||a>=b+borrow_bytes);
 }
 for(int i=0;i<n;i++)out[i]=(uint16_t)((in[i]>>8)|(in[i]<<8));
 copy_bytes+=(size_t)n*2;
}
static void swap_pie(const uint16_t *in,uint16_t *out,unsigned n){
 assert((uintptr_t)in%16==0&&(uintptr_t)out%16==0);
 swap_scalar(out,in,(int)n*16);
}
'''
CASES = r'''
static esp_err_t drain(void){
#ifdef KASANE_DMA_OVERLAP
 return board_present_finish();
#else
 return tx_reap();
#endif
}
static void reset(void){
 assert(drain()==ESP_OK);assert(!bus_pending);
 next_row=-1;capture=false;g_board_async=1;g_board_swap_into=1;pie_swap=false;
 commands=control_calls=queues=reaps=copies_while_pending=0;wire_bytes=copy_bytes=0;
 fault=queue_error=control_error_at=0;ramwr=false;
 for(int i=0;i<LCD_W*LCD_H;i++)glass[i]=expected[i]=0xdead;
}
static void fill(int y,int rows){
 for(int r=0;r<rows;r++)for(int x=0;x<LCD_W;x++)shared[r*LCD_W+x]=(uint16_t)(17+(y+r)*241+x);
}
static void expect(int x,int y,int cols,int rows){
 for(int r=0;r<rows;r++)for(int c=x;c<x+cols;c++)expected[(y+r)*LCD_W+c]=(uint16_t)(17+(y+r)*241+c);
}
static void send(int x,int y,int cols,int rows){
 fill(y,rows);expect(x,y,cols,rows);
 assert(board_present_rect(x,y,cols,rows,shared)==ESP_OK);
 memset(shared,0xa5,sizeof(shared));check_borrow();
}
static void verify(void){assert(drain()==ESP_OK);assert(memcmp(glass,expected,sizeof(glass))==0);}
int main(void){
 assert(sizeof(shared)==3840&&sizeof(tx_buf)==EXPECTED_TX_BYTES);
 assert(board_strip()==shared&&board_strip()==board_strip());
 assert(((uintptr_t)board_strip()%16)==0);
 for(int async=0;async<2;async++)for(int pie=0;pie<2;pie++){
  reset();g_board_async=async;pie_swap=pie;
  for(int y=0;y<LCD_H;y+=STRIP_H)send(16,y,32,LCD_H-y<STRIP_H?LCD_H-y:STRIP_H);
  verify();assert(next_row==-1&&wire_bytes==8640);
  assert(commands==(CONTINUATION?3:51));
  assert(control_calls==(CONTINUATION?5:85)+(async?0:17));
  printf("NARROW async=%d pie=%d commands=%u control_calls=%u payload=%zu tx=%zu shared=%zu\n",async,pie,commands,control_calls-(async?0:17),wire_bytes,sizeof(tx_buf),sizeof(shared));
 }
 reset();send(0,0,32,8);send(0,8,32,8);send(0,16,240,8);
 send(16,24,32,8);send(16,32,64,8);send(32,40,64,8);send(32,64,64,7);
 verify();assert(commands==(CONTINUATION?18:21));
 reset();pie_swap=true;send(3,0,19,8);send(3,8,19,7);verify();
 assert(commands==(CONTINUATION?3:6)&&wire_bytes==570);
 /* A logical frame end must not retain a partial window into the next frame. */
#if CONTINUATION
 reset();send(16,0,32,8);verify();assert(next_row==8);
 board_present_frame_end();assert(next_row==-1);
 unsigned n=commands;send(16,8,32,8);verify();assert(commands==n+3);
#endif
 /* Native copy overlap is deliberately removed only by the single-TX case.
  * Kasane submit already drains first, independently of TX buffer count. */
 reset();send(0,0,240,8);send(0,8,240,8);verify();
 assert(copies_while_pending==(EXPECTED_TX_BYTES==3840?0:1));
 printf("NATIVE copies_while_pending=%u tx=%zu\n",copies_while_pending,sizeof(tx_buf));
#ifdef KASANE_DMA_OVERLAP
 reset();fill(0,8);expect(0,0,240,8);
 assert(board_present_submit(0,0,240,8,shared)==ESP_OK);
 fill(8,8);expect(0,8,240,8);
 assert(board_present_submit(0,8,240,8,shared)==ESP_OK);verify();
 assert(copies_while_pending==0);
#endif
 /* Full-width streaming remains 3 commands for all 17 bands, including tail. */
 reset();for(int y=0;y<LCD_H;y+=8)send(0,y,240,LCD_H-y<8?LCD_H-y:8);
 verify();assert(commands==3&&wire_bytes==64800&&next_row==-1);
 /* Native UI keeps its shared pointer; switching async/polling drains first. */
 reset();send(0,0,240,8);g_board_async=0;send(16,8,32,8);verify();
 g_board_async=1;g_board_swap_into=0;send(0,16,240,8);verify();
 for(int f=1;f<=4;f++){
  reset();send(0,0,240,8);assert(bus_pending&&tx_inflight);
  const void *old=tx_pending.tx_buffer;spi_transaction_t descriptor=tx_pending;
  fault=f;fill(8,8);
  esp_err_t e=board_present(8,8,shared);
  assert(e==(f==1?ESP_ERR_TIMEOUT:f==2?ESP_ERR_INVALID_ARG:ESP_ERR_INVALID_STATE));
  assert(next_row==-1);check_borrow();
  assert(tx_pending.tx_buffer==old&&memcmp(&descriptor,&tx_pending,sizeof(descriptor))==0);
  assert(bus_pending==(f!=4)&&tx_inflight==(f!=4));
  if(f!=4){
   fault=2;unsigned q=queues,c=commands;
   assert(board_present_rect(16,8,32,8,shared)==ESP_ERR_INVALID_ARG);
   assert(queues==q&&commands==c&&bus_pending&&tx_inflight);check_borrow();
  }
  assert(drain()==ESP_OK&&!bus_pending&&!tx_inflight);
  next_row=-1;send(0,0,240,8);verify();
 }
 /* Every CASET/RASET/RAMWR command or parameter failure invalidates B. */
 for(int step=1;step<=5;step++){
  reset();control_error_at=step;fill(0,8);
  assert(board_present_rect(16,0,32,8,shared)==25);
  assert(next_row==-1&&!bus_pending&&!tx_inflight);
  control_error_at=0;send(16,0,32,8);verify();
 }
 reset();send(16,0,32,8);fault=2;fill(8,8);
 assert(board_present_rect(16,8,32,8,shared)==ESP_ERR_INVALID_ARG);
 assert(next_row==-1&&bus_pending&&tx_inflight);check_borrow();
 assert(drain()==ESP_OK);send(16,8,32,8);verify();
 reset();queue_error=1;fill(0,8);
 assert(board_present(0,8,shared)==31&&!tx_inflight&&!bus_pending&&next_row==-1);
 send(0,0,240,8);verify();
 /* Capture output remains unswapped, full-row RGB565; toggles invalidate B. */
 reset();send(0,0,240,8);verify();unsigned n=commands;
 board_capture(true);assert(board_capture_take_turn());assert(!board_capture_take_turn());
 send(0,8,240,7);verify();
 assert(commands==n+(CONTINUATION?3:0));board_capture(false);
#if CONTINUATION
 assert(next_row==-1);
#endif
 puts("BOARD_TRANSPORT PASS: native/DMA OFF+ON ownership, stable scratch, packed panel pixels, windows, tail, capture, faults");
}
'''
parts = [function(x) for x in ('uint16_t *board_strip(', 'static esp_err_t tx_reap(',
    'void board_capture(', 'bool board_capture_take_turn(', 'static esp_err_t command(',
    'esp_err_t board_present(', 'esp_err_t board_present_rect(')]
if CONTINUATION:
    parts.append(function('void board_present_frame_end('))
with tempfile.TemporaryDirectory(prefix='board-transport-') as tmp:
    folder = Path(tmp)
    for dma in (False, True):
        functions = parts + ([function('esp_err_t board_present_finish('), function('esp_err_t board_present_submit(')] if dma else [])
        code = (HEADER.replace('@DECLARATIONS@', DECLARATIONS)
                + '\n'.join(functions) + CASES)
        path = folder / 'test.c'
        path.write_text(code)
        for flags in (['-O2', '-fstrict-aliasing'], ['-g', '-fsanitize=address,undefined']):
            exe = folder / 'test'
            subprocess.run([os.environ.get('CC', 'cc'), '-std=c11', '-Wall', '-Wextra', '-Werror',
                            f'-DEXPECTED_TX_BYTES={EXPECTED_TX_BYTES}',
                            f'-DCONTINUATION={int(CONTINUATION)}',
                            *(['-DKASANE_DMA_OVERLAP=1'] if dma else []), *flags,
                            str(path), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            if result.returncode:
                raise SystemExit(result.stdout + result.stderr)
            pix = [line for line in result.stdout.splitlines() if line.startswith('PIX ')]
            assert len(pix) == 7
            for row, line in enumerate(pix, 8):
                assert line == f'PIX {row} ' + ''.join(f'{(17+row*241+x)&65535:04x}' for x in range(240))
            print(json.dumps(dict(dma=dma, flags=flags, tx_bytes=EXPECTED_TX_BYTES,
                                  capture_rows=len(pix), continuation=CONTINUATION)))
            print('\n'.join(line for line in result.stdout.splitlines()
                            if line.startswith(('NARROW ', 'NATIVE ', 'BOARD_TRANSPORT '))))
