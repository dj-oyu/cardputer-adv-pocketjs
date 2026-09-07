#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
// Nine chapters today, so the lesson slots are 1..9 and the tail of the table
// is free. pocket_workspace.c hands those out as the works library; the
// Playground's own slot 0 appears there too, because it is the person's work
// like any other. Growing the tutorial past six more chapters would need this
// boundary moved and every work below it rehomed, which is why the two numbers
// are here together rather than one of them being counted somewhere else.
#define SRC_SLOT_WORK       10
#define SRC_SLOT_WORK_COUNT 6
#define SRC_SLOT_COUNT    16

// Fills `out` (SRC_MAX+1 bytes, NUL terminated) and returns its length.
// Returns 0 when the slot is empty or its record does not verify.
size_t srcstore_load(unsigned slot, char *out);

// The same read, with the two zero-length answers told apart: `verified` is
// true when a record was read whole, so a saved empty document (length 0,
// verified) is distinguishable from a slot with nothing in it and from one
// whose faces both failed their CRC (length 0, not verified). The editor does
// not care -- an empty buffer is an empty buffer -- but an API that must keep
// an empty document and must report a corrupt one does.
size_t srcstore_load_checked(unsigned slot, char *out, bool *verified);

bool srcstore_save(unsigned slot, const char *text, size_t len);

// Forgets a slot. Used to reset one chapter without touching the others.
bool srcstore_clear(unsigned slot);

// The sequence number of the newest good record in a slot, or 0 when the slot
// holds nothing. Every save increments it, so it is already the revision
// docs/common-api.md section 7 asks a work to carry — and being the store's own
// counter rather than a number kept beside it, it stays right when the native
// Playground saves the same slot behind the workspace's back.
uint32_t srcstore_revision(unsigned slot);
