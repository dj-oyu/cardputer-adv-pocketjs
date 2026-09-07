#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The Playground's editing model, with no screen, no dictionary and no flash in
// it. It works on a buffer the caller owns, which is the point: the editor
// cannot be run anywhere but the board, while this can be run against a
// malloc'd document on a host and asserted (tools/test_vimcmd.c).

// The unnamed register. Three 40-column lines are ~120 bytes, so this is
// generous; a yank larger than it is refused rather than truncated, because a
// truncated paste is a wrong program that looks like a right one.
#define VIM_REG_MAX    512
// Undo keeps byte ranges, not snapshots — a second copy of an 8 KB document
// does not fit in this machine. Deleted text is the only thing that has to be
// remembered, so this ring plus the record table is the whole cost.
#define VIM_UNDO_BYTES 512
#define VIM_UNDO_RECS  32
#define VIM_CMD_MAX    48
#define VIM_PAT_MAX    32
#define VIM_MSG_MAX    40
#define VIM_FIND_MAX   8       // one UTF-8 character for f/F/t/T

typedef enum { VIM_NORMAL = 0, VIM_INSERT, VIM_CMDLINE } vim_mode_t;

// What the engine cannot do for itself: flash and the screen belong to the
// editor. Everything else the engine finishes on its own.
typedef enum {
    VIM_ACT_NONE = 0,
    VIM_ACT_SAVE,         // :w
    VIM_ACT_QUIT,         // :q  — the editor refuses it while the buffer is dirty
    VIM_ACT_QUIT_FORCE,   // :q!, and Esc from a normal mode with nothing pending
    VIM_ACT_SAVE_QUIT,    // :wq, :x, ZZ
} vim_action_t;

// The document. `text` is borrowed and stays NUL terminated; `cap` is the most
// bytes it may hold, not counting that NUL.
typedef struct {
    char  *text;
    size_t len, cap, cursor;
    bool   changed;       // every mutation sets it; the editor clears it on save
} vim_doc_t;

// A keystroke, already stripped of the board's "\0name" encoding. TEXT carries
// UTF-8, which may be several bytes when the IME committed a word.
typedef enum {
    VIM_KEY_TEXT = 0, VIM_KEY_ESC, VIM_KEY_ENTER, VIM_KEY_BACKSPACE,
    VIM_KEY_LEFT, VIM_KEY_RIGHT, VIM_KEY_UP, VIM_KEY_DOWN
} vim_key_kind_t;

typedef struct {
    vim_key_kind_t kind;
    const char    *text;
    size_t         len;
} vim_key_t;

enum { VIM_UNDO_INSERTED = 0, VIM_UNDO_DELETED };

// One reversible step. `payload` indexes the deleted-text ring for a DELETED
// record and is unused for an INSERTED one, which only has to be cut back out.
// `seq` groups the steps of one change, so a whole insert-mode session comes
// back with a single `u`, as vim does.
typedef struct {
    uint16_t pos, len, payload;
    uint8_t  kind, seq;
} vim_undo_t;

typedef struct {
    uint8_t  mode;
    uint8_t  seq;
    bool     in_undo;

    // The half-typed command: counts either side of an operator, the operator
    // itself, and the key still waiting for its argument ('g', 'Z', 'f', 'F',
    // 't', 'T', 'r').
    uint32_t count1, count2;
    char     op, pending;

    // The last f/F/t/T, for `;` and `,`.
    char     find_kind;
    char     find_ch[VIM_FIND_MAX];
    uint8_t  find_len;

    // The line being typed after ':', '/' or '?'.
    char     cmdline[VIM_CMD_MAX];
    uint8_t  cmd_len;
    char     cmd_kind;

    char     pattern[VIM_PAT_MAX];
    uint8_t  pat_len;
    int8_t   search_dir;

    char     reg[VIM_REG_MAX];
    uint16_t reg_len;
    bool     reg_linewise;

    // The column j/k aims for, so a walk down a ragged file comes back out to
    // where it started. (uint16_t)-1 means "take it from the cursor".
    uint16_t want_col;

    vim_undo_t rec[VIM_UNDO_RECS];
    uint8_t    nrec;
    uint8_t    ubuf[VIM_UNDO_BYTES];
    uint16_t   ulen;

    char     msg[VIM_MSG_MAX];
    char     echo[12];
} vim_state_t;

// Back to a normal mode with nothing pending and no undo history. The register
// and the search pattern survive, as they do across files in vim.
void vim_reset(vim_state_t *v);

// One key. Returns what the editor still owes.
vim_action_t vim_feed(vim_state_t *v, vim_doc_t *d, vim_key_t k);

vim_mode_t vim_mode(const vim_state_t *v);

// Put the cursor on a character rather than past one. The editor calls this
// after loading a document, which leaves the cursor at the end.
void vim_clamp(const vim_state_t *v, vim_doc_t *d);

// Start typing without a command — what "empty the document" means.
void vim_begin_insert(vim_state_t *v);

// "" when the last key had nothing to say.
const char *vim_message(const vim_state_t *v);

// The ':' or '/' line as typed, leader included; NULL outside VIM_CMDLINE.
const char *vim_cmdline(const vim_state_t *v, size_t *len);

// The half-typed command, for the corner of the footer: "3d", "d2f", "".
const char *vim_pending(vim_state_t *v);
