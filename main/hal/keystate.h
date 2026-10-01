#pragma once
#include <stdbool.h>
#include <stdint.h>

// Which physical keys are down, as opposed to what keymap.c made of them.
//
// keymap_poll() is the one reader of the keypad FIFO (a read consumes the
// event), and it translates for the shell: it drops every non-modifier release
// and reduces a press to one keystroke. That is the right shape for text and
// menus and the wrong one for a game, which needs "is E still down" and "did D
// go down since the last frame". So keymap_poll() also reports every raw edge
// here, before it translates anything, and this file keeps the answer for any
// task to read. One writer (the input task, through keymap_poll), any number of
// readers; see keystate.c for why a snapshot is consistent.
//
// A key is its matrix cell, index row*14+col (4 rows x 14 columns, the layout
// keymap.c remaps the TCA8418's 7x8 into). Names live in keymap.c beside the
// character table they are derived from.
#define KEYSTATE_ROWS 4
#define KEYSTATE_COLS 14
#define KEYSTATE_KEYS (KEYSTATE_ROWS*KEYSTATE_COLS)
#define KEYSTATE_WORDS ((KEYSTATE_KEYS+31)/32)

typedef struct {
    uint32_t held[KEYSTATE_WORDS];      // bit (i&31) of word i>>5: key i is down
    // Per-key edge counters, modulo 256. A reader keeps the previous snapshot
    // and compares, so a press and release that both land between two reads
    // still show as one of each. 256 edges of ONE key between two reads would
    // alias to none; the input task drains one event per 5 ms poll, so that is
    // 1.28 s of nothing but that key with no reader, which the readers here
    // (one per frame, re-based across every gap -- pocket_input.c) never allow.
    uint8_t  presses[KEYSTATE_KEYS];
    uint8_t  releases[KEYSTATE_KEYS];
    uint32_t edges;                     // every edge applied, all keys
    uint32_t overflows;                 // FIFO overflows seen (keystate_overflow)
} keystate_snapshot_t;

// Writer side: the input task only, through keymap_poll().
//
// A press of a key already down, or a release of one already up, changes
// nothing and counts nothing: edges alternate per key by construction, which
// is what lets an overflow (below) resolve to "released" and stay consistent
// when the real release arrives later.
void keystate_apply(int row, int col, bool pressed);
// The controller dropped events (FIFO full). Which keys went up or down in the
// lost ones is unknowable -- the TCA8418 has no register for the current state
// of a matrix key -- so every key that is down is released, with a counted
// release edge. A key that is really still held reads as up until it is
// pressed again: a game character stops rather than running on forever.
void keystate_overflow(void);

// Reader side: any task. The copy is one consistent state: never half of one
// edge and half of the next.
void keystate_snapshot(keystate_snapshot_t *out);

static inline bool keystate_bit(const uint32_t *words, int index) {
    return (words[index>>5]>>(index&31))&1u;
}
