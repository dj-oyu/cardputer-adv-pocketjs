#!/bin/bash
# Host preview of the FLOWER shot list, and of alternatives to it. Nothing here
# touches the board.
#
#   bash tools/preview_flower_shots.sh              # every variant
#   bash tools/preview_flower_shots.sh live         # just the shipping table
#
# A VARIANT is a whole flower_shots.h with its FLOWER_SHOTS table replaced. Each
# one is written into .cache/variants/<name>/ next to a copy of flower.c, so the
# quoted #include "flower_shots.h" in flower.c resolves to that variant and
# nothing in main/ is touched or rebuilt. The variants exist so the options can
# be compared in a picture rather than argued about in numbers; which of them is
# right is a question for whoever is looking at the sheets.
set -e
cd "$(dirname "$0")/.."

# Splice a table body into a copy of the live header. Taking the table from
# stdin rather than sed-ing individual numbers means a variant can change the
# SHOT COUNT, which is the thing that actually moved today.
variant() {                      # variant <name>   [table body on stdin]
    local name=$1
    local dir=.cache/variants/$name
    mkdir -p "$dir" ".cache/var-$name"
    cp main/scene/flower.c "$dir/flower.c"
    # Read the body BEFORE anything else touches stdin: the splicer below is
    # itself a heredoc, and it would otherwise consume this one.
    local body=""
    [ "$name" = live ] || body=$(cat)
    if [ -z "$body" ]; then
        cp main/scene/flower_shots.h "$dir/flower_shots.h"
    else
        PFS_BODY="$body" python3 tools/pfs_splice.py "$dir/flower_shots.h"
        cmp -s "$dir/flower_shots.h" main/scene/flower_shots.h &&
            { echo "variant $name changed nothing"; exit 1; }
    fi
    gcc -O2 -Wall -Wextra -Werror -I"$dir" -Imain/scene \
        -DPFS_FLOWER_SRC="\"../$dir/flower.c\"" \
        tools/preview_flower_shots.c -lm -o ".cache/preview-$name"
    "./.cache/preview-$name" ".cache/var-$name"
    python3 tools/preview_flower_shots.py "$name"
}

only=${1:-}
want() { [ -z "$only" ] || [ "$only" = "$1" ]; }

# The table as it ships: one flower, one cut, at the cheapest rung.
want live && variant live </dev/null || true

# The framings worth looking at, each as THE single shot. A framing is a zoom
# AND an aim: the pair decides both what is in the window and what it costs.
want tight && variant tight <<'EOF' || true
    { -0.34f, 2.20f, 0.94f, 40.0f },
EOF
want middle && variant middle <<'EOF' || true
    { -0.32f, 1.55f, 0.72f, 40.0f },
EOF
want constant-wide && variant constant-wide <<'EOF' || true
    { -0.05f, 1.00f, 0.50f, 40.0f },
EOF
want low && variant low <<'EOF' || true
    { -0.24f, 0.00f, 0.50f, 40.0f },
EOF

[ -z "$only" ] && python3 tools/preview_flower_shots.py --compare || true
