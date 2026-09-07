#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "skk_core.h"   // SKK_MODE_*, which the footer names

// A stand-in for the IME, small enough that a host test can put it in any
// state the editor draws differently: composing, converting, off. The real
// ime_t embeds a skk_t and a mapped dictionary partition, neither of which a
// host has.
typedef enum { IME_NONE = 0, IME_TAKEN, IME_TEXT } ime_disp_t;

typedef struct {
    bool on;
    int  mode;
    char preedit[64]; size_t preedit_len;
    char commit[64];  size_t commit_len;
    const char *cand[8]; size_t cand_len[8];
    int  ncand, sel;
    ime_disp_t next;          // what the next ime_feed() will answer
} ime_t;

bool skk_session_ready(void);
ime_t *skk_session(void);

bool ime_on(const ime_t *im);
int  ime_mode(const ime_t *im);
void ime_set_on(ime_t *im, bool on);
void ime_reset(ime_t *im);
ime_disp_t ime_feed(ime_t *im, const char *key, size_t len);
const char *ime_text(const ime_t *im, size_t *len);
const char *ime_preedit(const ime_t *im, size_t *len);
int  ime_cand_count(const ime_t *im);
int  ime_sel(const ime_t *im);
const char *ime_cand(const ime_t *im, int i, size_t *len);

// Test controls, not part of the device's surface.
void host_ime_present(bool present);
void host_ime_preedit(const char *utf8);
void host_ime_candidates(const char *const *list, int n, int sel);
