#pragma once
#include <stdbool.h>
#include <stdint.h>

// One UI-task owner; analytic parts and row scratch live in scene_mem.
//
// FLOWER_CRYSTAL is not reachable from the menu, and that is not an oversight.
// It is the first flower that was built and it is the fixture the host test
// measures against: an exact analytic shape whose surface points can be checked
// against the quadratic that defines them, which no botanical has. It shares
// the same part list and the same scene block as the others, so keeping it
// costs no DRAM -- only the branch that builds it.
typedef enum {
    FLOWER_CRYSTAL, FLOWER_VALLEY, FLOWER_SUNFLOWER, FLOWER_SNOWDROP,
    FLOWER_TULIP, FLOWER_DAFFODIL, FLOWER_CROCUS, FLOWER_CALLA,
    FLOWER_SPECIES_COUNT
} flower_species_t;
// Draw one named species, with no dissolve. What the tests and the fixture use.
void flower_prepare(float dt, int tilt_x, int tilt_y, flower_species_t species);
// What the single FLOWER menu row uses: rotates the botanicals, hiding
// each change behind a dissolve. CRYSTAL is not in the rotation.
void flower_prepare_rotating(float dt, int tilt_x, int tilt_y);
// The rotation's state, for the host test. flower_fade() is 1 for a fully drawn
// plant and 0 at the frame a species is replaced.
flower_species_t flower_current_species(void);
float flower_fade(void);
void flower_draw(uint16_t *pixels, int y, int height);
