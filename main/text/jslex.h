#pragma once
#include <stddef.h>
#include <stdint.h>

// A colouring lexer for the editor. Not a parser: it answers "what colour is
// this byte", nothing else, and it never stops on malformed input the way
// QuickJS's own tokenizer does — an unterminated string simply ends at the
// newline, which is the state an editor spends most of its time in.
//
// Allocates nothing. The caller receives runs through a callback and decides
// what to keep, so the lexer itself holds no table of any size.
typedef enum {
    JSLEX_PLAIN = 0,   // identifiers, operators, whitespace
    JSLEX_KEYWORD,
    JSLEX_STRING,      // '...' "..." `...` and regexp literals
    JSLEX_COMMENT,
    JSLEX_NUMBER,
    JSLEX_KINDS
} jslex_kind_t;

// One run of same-coloured bytes, never crossing a line boundary.
// `off` is from the start of `text`; `line` is 0-based.
typedef void (*jslex_emit_fn)(void *user_data, unsigned line,
                              size_t off, size_t len, uint8_t kind);

// Scans from the beginning — the state at any line depends on everything
// before it — but reports only lines in [from_line, to_line] and stops once
// the window is behind it.
void jslex_scan(const char *text, size_t len,
                unsigned from_line, unsigned to_line,
                jslex_emit_fn emit, void *user_data);
