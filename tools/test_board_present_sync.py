"""Exercise the real board transfer barrier against a fault-injected SPI stub.

The board's ESP-IDF setup is not host-buildable; extract only this owner-task
boundary. This checks acknowledgement ordering, not SPI hardware behaviour.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "main/hal/board.c").read_text(encoding="utf-8")
start = source.index("esp_err_t board_present_sync(")
end = source.index("\n}", start) + 2
boundary = source[start:end]
harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
static int next_row, pending, fail_prior, fail_send, fail_completion, sends;
static uint16_t pixels[240*8];
static esp_err_t tx_reap(void) {
    if(!pending) return ESP_OK;
    pending=0;
    if(fail_prior) { fail_prior=0; return 17; }
    if(fail_completion) { fail_completion=0; return 19; }
    return ESP_OK;
}
static esp_err_t board_present(int y,int rows,uint16_t *p) {
    assert(!pending); assert(p==pixels); assert(y==8 && rows==8);
    sends++;
    if(fail_send) return 18;
    pending=1; next_row=y+rows; return ESP_OK;
}
'''
cases = r'''
int main(void) {
    /* An earlier screen's pending failure must not be swallowed by commands. */
    next_row=8; pending=1; fail_prior=1;
    assert(board_present_sync(8,8,pixels)==17);
    assert(sends==0 && !pending && next_row==-1);
    /* Queue rejection and last-strip completion failure must both propagate. */
    fail_send=1;
    assert(board_present_sync(8,8,pixels)==18 && next_row==-1);
    fail_send=0; fail_completion=1;
    assert(board_present_sync(8,8,pixels)==19);
    assert(!pending && next_row==-1);
    /* Successful retry acknowledges completed pixels and preserves continuity. */
    assert(board_present_sync(8,8,pixels)==ESP_OK);
    assert(!pending && next_row==16);
    pending=1;
    assert(board_present_sync(8,8,pixels)==ESP_OK && !pending);
    puts("board transfer barrier: PASS");
}
'''
with tempfile.TemporaryDirectory(prefix="kasane-board-sync-") as temp:
    src = Path(temp) / "test.c"
    exe = Path(temp) / "test"
    src.write_text(harness + boundary + cases, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall",
                    "-Wextra", "-Werror", str(src), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
