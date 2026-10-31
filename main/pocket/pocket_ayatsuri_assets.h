#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Process-lifetime immutable asset span; never a mutable JS backing store. */
bool pocket_ayatsuri_builtin_asset(const char *name, const uint8_t **data, size_t *bytes);
