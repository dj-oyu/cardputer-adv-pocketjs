#pragma once
#include "../main/scene/flower.h"
// Host-only labels: no strings are embedded in the firmware for previews.
static const char *const flower_names[]={
    "valley", "sunflower", "snowdrop", "tulip", "daffodil", "crocus", "calla",
    "platycodon", "echinacea", "anemone", "nigella", "aquilegia", "fritillaria", "iris"
};
_Static_assert(sizeof flower_names/sizeof flower_names[0]==FLOWER_SPECIES_COUNT,
               "update host labels when adding a botanical");
