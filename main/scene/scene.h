#pragma once
#include <stdint.h>

// What a home-screen background is, as one row of a table.
//
// The same argument main.c makes for SCREENS[], one layer down. The shell used
// to ask `if(mode==0)` in the prepare block, `if(mode==1)` in the strip loop,
// `if(mode<2)` for the stars and `if(mode==2)` for the clock label -- four
// chains in three places, each of which had to be edited, correctly, to add a
// background. Two of them tested `mode>=3` for "is a flower", which is a fact
// about the order of the menu rather than about the scene.
//
// A row says what a scene is instead, and shell_draw does the repeated parts
// once. Adding a background is adding a row; dropping one, or renaming one, or
// deciding that four flower rows are really one, is editing the table and
// nothing else. tools/home_modes.py reads the same rows for the USB tests, so
// the menu length the scripts count key presses against cannot drift from the
// list the firmware draws.
typedef struct {
    // The BACKGROUND settings row, and what tools/home_modes.py extracts. The
    // settings table points `values` straight at this field and walks it by
    // sizeof(scene_ops_t), so there is no parallel array of labels to drift.
    const char *name;
    // Advance one frame. `variant` is the row's own, which is how four flower
    // rows share one renderer without the renderer knowing what a menu is.
    void (*prepare)(float dt, int tilt_x, int tilt_y, unsigned variant);
    // Paint one strip, `height` rows starting at `y`. Returns the cycles its
    // vector kernel spent, which is not the same as the cycles the call spent:
    // PERF's kernel= has always meant the vector rows alone, and the sky above
    // the ocean's horizon and the scaffolding around the loop are a quarter of
    // the frame that is deliberately not in it. A scene with no kernel returns
    // 0 rather than being asked to guess.
    uint32_t (*draw)(uint16_t *strip, int y, int height);
    // Drawn over the strip after draw(), for the things that need the shell's
    // font and pixel writer rather than the scene's: the sail's clock label,
    // the stars. NULL when the scene has none.
    void (*overlay)(uint16_t *strip, int y, int height);
    // Passed to prepare() unchanged. Meaningless to a scene that ignores it.
    unsigned variant;
} scene_ops_t;
