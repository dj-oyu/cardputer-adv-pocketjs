#pragma once
#include <stdbool.h>

/* Diagnostic image only. Call from the FLOWER prepare path before the normal
 * candidate capture. Returns true while its fixed species sequence is active.
 * It leaves the selected species prepared for the usual shell present path. */
bool flower_d1_species_prepare(void);
