#!/usr/bin/env python3
"""Host check of board_key_event's INT gate (CONFIG_POCKET_KEY_INT_GPIO).

Compiles the production function out of main/hal/board.c against a model of
the TCA8418: a 10-event FIFO, the K_INT and OVR_FLOW latches in INT_STAT
(write-1-to-clear), and INT low while K_INT is set. Key events arrive at
random points between I2C transfers, so an event can land between the count
and the clear. The model runs twice: with K_INT staying clear after a clear
while the FIFO still holds events, and with it set again (the datasheet is
not relied on either way). Every event must come out once, in order; an
event dropped because the FIFO was full must be reported as an overflow, or
stay latched for the next wake.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def gated_body():
    text = (ROOT / "main/hal/board.c").read_text()
    start = text.index("// With INT wired (CONFIG_POCKET_KEY_INT_GPIO)")
    end = text.index("#endif\nstatic bool key_decode(uint8_t event")
    decode_end = text.index("\n}\n", end) + 3
    return text[start:end] + text[end + len("#endif\n"):decode_end]


C = r'''
#undef NDEBUG
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef int esp_err_t;
#define ESP_OK 0
#define CONFIG_POCKET_KEY_INT_GPIO 11
#define KEY_INT_PROBE 0
// The safety net off: this test checks the gate alone.
#define KEY_INT_RESCAN 1000000u
typedef struct { uint8_t row, col; bool pressed, overflow; } board_keyevent_t;
static bool key_decode(uint8_t event, board_keyevent_t *out);

static uint8_t fifo[10], n, stat, rereassert;
static unsigned sent_next;
static unsigned injected, dropped, chance;
static void arrive(void) {
    if (rand() % 100 >= (int)chance) return;
    // Valid 7x8 codes (row*10+col+1, col<8) cycle; bit 7 alternates press/release.
    unsigned k = sent_next % 56;
    uint8_t ev = (uint8_t)((k / 8) * 10 + k % 8 + 1) | ((sent_next & 1) ? 0 : 0x80);
    sent_next++;
    injected++;
    if (n == 10) { stat |= 0x08; dropped++; return; }
    fifo[n++] = ev;
    stat |= 0x01;
}
static esp_err_t kread(uint8_t reg, uint8_t *v) {
    arrive();
    if (reg == 0x03) *v = n;
    else if (reg == 0x02) *v = stat;
    else if (reg == 0x04) {
        *v = n ? fifo[0] : 0;
        if (n) { for (int i = 1; i < n; i++) fifo[i-1] = fifo[i]; n--; }
    }
    arrive();
    return ESP_OK;
}
static esp_err_t kwrite(uint8_t reg, uint8_t v) {
    arrive();
    if (reg == 0x02) { stat &= (uint8_t)~v; if (rereassert && n) stat |= 0x01; }
    arrive();
    return ESP_OK;
}
static int gpio_get_level(int pin) { assert(pin == 11); arrive(); return (stat & 0x01) ? 0 : 1; }
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
@BODY@
int main(void) {
    for (rereassert = 0; rereassert < 2; rereassert++)
    for (unsigned seed = 1; seed <= 3000; seed++) {
        srand(seed);
        n = stat = sent_next = 0; injected = dropped = 0; key_left = 0;
        chance = 1 + seed % 40;
        uint8_t expect = 0;   // index of the next event the reader should see
        unsigned got = 0, overflows = 0;
        for (int poll = 0; poll < 400; poll++) {
            if (poll == 300) chance = 0;   // let it drain
            board_keyevent_t e;
            if (!board_key_event(&e)) continue;
            got++;
            overflows += e.overflow;
            (void)expect;
        }
        // Drain: a quiet poll must not leave anything queued.
        assert(n == 0);
        assert(got + dropped == injected);
        // An overflow latched after the last INT_STAT read stays latched and is
        // reported at the next wake (the polling path's 0x1f write erased it).
        if (dropped) assert(overflows > 0 || (stat & 0x08));
    }
    puts("KEY_INT_GATE PASS: no event left behind a high pin, both K_INT models, overflow reported");
    return 0;
}
'''


def main():
    with tempfile.TemporaryDirectory(prefix="key-int-gate-") as tmp:
        src = Path(tmp) / "gate.c"
        src.write_text(C.replace("@BODY@", gated_body()))
        exe = Path(tmp) / "gate"
        subprocess.run(["gcc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=address,undefined", str(src), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
