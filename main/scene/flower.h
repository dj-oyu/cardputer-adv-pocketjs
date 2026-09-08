#pragma once
#include <stdbool.h>
#include <stdint.h>

// One UI-task owner; analytic parts and row scratch live in scene_mem.
//
// Adding a species is two edits and neither is in the renderer: a row in the
// enum below, and a branch in flower_build_botanicals() in
// main/scene/flower_species.c. That file is the whole vocabulary -- stems,
// bells, trumpets, cups -- and it is meant to be readable without flower.c,
// which is a ray tracer and has nothing to say about plants. The rotation
// picks up a new row on its own; nothing else needs telling.
//
// Every entry represents a real botanical species. See docs/flower-home.md
// for the scientific names and the features retained at display resolution.
typedef enum {
    FLOWER_VALLEY, FLOWER_SUNFLOWER, FLOWER_SNOWDROP,
    FLOWER_TULIP, FLOWER_DAFFODIL, FLOWER_CROCUS, FLOWER_CALLA,
    FLOWER_PLATYCODON, FLOWER_ECHINACEA, FLOWER_ANEMONE, FLOWER_NIGELLA,
    FLOWER_AQUILEGIA, FLOWER_FRITILLARIA, FLOWER_IRIS,
    FLOWER_SPECIES_COUNT
} flower_species_t;
// Draw one named species, with no dissolve. What the tests and the fixture use.
void flower_prepare(float dt, int tilt_x, int tilt_y, flower_species_t species);
// What the single FLOWER menu row uses: rotates the botanicals, hiding
// each change behind a dissolve.
void flower_prepare_rotating(float dt, int tilt_x, int tilt_y);
// The rotation's state, for the host test. flower_fade() is 1 for a fully drawn
// plant and 0 at the frame a species is replaced.
flower_species_t flower_current_species(void);
float flower_fade(void);
void flower_draw(uint16_t *pixels, int y, int height);
// The grain's phase. It advances once a frame, so two renders of the same
// moment only match if it is pinned -- which is what a test comparing them has
// to do, and what nothing else should touch.
extern unsigned flower_grain_frame;
