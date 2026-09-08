#!/bin/bash
# Prices every zoom rung as the single shot. Host only; no board, no build/.
#
# The live flower_shots.h holds ONE shot now, so the rungs being compared do not
# exist in the tree any more. They are synthesized into a variant header here --
# four entries, equal dwell so the sum still makes 40 s -- and flower.c is
# compiled against that copy. main/ is not touched.
set -e
cd "$(dirname "$0")/.."
dir=.cache/variants/rungs
mkdir -p "$dir"
cp main/scene/flower.c "$dir/flower.c"
sed -n '1,/^#define FLOWER_PITCH_MAX/p' main/scene/flower_shots.h > "$dir/flower_shots.h"
cat >> "$dir/flower_shots.h" <<'EOF'
// Synthesized by tools/flower_shot_cost.sh. Framings, not rungs: cost follows
// how much plant is inside the window, so a zoom without its aim prices
// nothing. Equal dwell keeps the sum at the interval. Exists to price, not to
// ship.
typedef struct { float pitch,zoom,aim,hold; } flower_shot_t;
static const flower_shot_t FLOWER_SHOTS[]={
    { -0.05f, 0.00f, 0.50f, 8.0f },   /* fitted wide */
    { -0.05f, 1.00f, 0.50f, 8.0f },   /* constant wide */
    { -0.32f, 1.55f, 0.72f, 8.0f },   /* the middle */
    { -0.32f, 1.90f, 0.90f, 8.0f },   /* tighter, aim raised */
    { -0.34f, 2.20f, 0.94f, 8.0f },   /* tight, aim on the head */
};
#define FLOWER_VIEWS ((int)(sizeof FLOWER_SHOTS/sizeof FLOWER_SHOTS[0]))
#define FLOWER_VIEW_MIN_S 8.0f
EOF
gcc -O2 -Wall -Wextra -Werror -I"$dir" -Imain/scene tools/flower_shot_cost.c -lm \
    -o .cache/flower_shot_cost
./.cache/flower_shot_cost
