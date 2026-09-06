#pragma once
#include <stdbool.h>
#include <stddef.h>

// JavaScript sources in the storage partition. Flat records rather than a
// filesystem: one program is edited at a time, and a record needs no mount, no
// allocator and no wear-levelling metadata.
#define SRC_MAX 8192

// Slots are independent. The Playground's is the one the person types their
// own programs into, and nothing else may write to it — a tutorial loading a
// worked example must not cost someone the program they were keeping.
#define SRC_SLOT_USER     0
// One per tutorial chapter, so returning to a chapter shows what was left
// there rather than the template again.
#define SRC_SLOT_LESSON   1
#define SRC_SLOT_COUNT    16

// Fills `out` (SRC_MAX+1 bytes, NUL terminated) and returns its length.
// Returns 0 when the slot is empty or its record does not verify.
size_t srcstore_load(unsigned slot, char *out);

bool srcstore_save(unsigned slot, const char *text, size_t len);

// Forgets a slot. Used to reset one chapter without touching the others.
bool srcstore_clear(unsigned slot);
