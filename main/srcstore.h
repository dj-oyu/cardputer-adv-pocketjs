#pragma once
#include <stdbool.h>
#include <stddef.h>

// One JavaScript source, kept in the storage partition. A single slot rather
// than a filesystem: the Playground edits one program at a time, and a flat
// record needs no mount, no allocator and no wear-levelling metadata.
#define SRC_MAX 8192

// Fills `out` (SRC_MAX+1 bytes, NUL terminated) and returns its length.
// Returns 0 when nothing has been saved or the record does not verify.
size_t srcstore_load(char *out);

bool srcstore_save(const char *text, size_t len);
