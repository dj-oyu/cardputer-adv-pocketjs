#!/usr/bin/env bash
# Candidate 4a (docs/perf/kasane-blend-pie.md): the PIE blend kernel's arm must
# move no pixel against the arms it replaces in main/ui/kasane/ksn_render.c.
#
# The switch is a runtime int (g_ksn_blend_pie, default 0), so the "on" build is
# the same sources plus a constructor that forces it to 1 before main: the two
# binaries differ only in which arm runs, which is the point. A build or a run
# that does not succeed is a FAILURE here -- never a silent "identical", because
# two empty logs also compare equal.
#
# Usage: bash tools/kasane_contract/run_blend_pie_ab.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT

cat > "$out/force.c" <<'EOF'
/* Force the kernel arm on: same sources, one switch, so the A/B is the arm. */
extern int g_ksn_blend_pie;
__attribute__((constructor)) static void ksn_force_blend_pie(void){ g_ksn_blend_pie=1; }
EOF

# $1 = tag, $2 = the harness (it includes ksn_render.c), rest = extra flags
ab() {
  tag=$1; src=$2; shift 2
  for arm in off on; do
    extra=""
    [ "$arm" = on ] && extra="$out/force.c"
    cc -std=c11 -Wall -Wextra -Werror -O2 -fstrict-aliasing -Imain/ui/kasane -Itools/kasane_contract \
       main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c main/ui/kasane/ksn_blend_pie.c \
       $extra "$src" "$@" -o "$out/$tag.$arm"
    "$out/$tag.$arm" > "$out/$tag.$arm.log"
    [ -s "$out/$tag.$arm.log" ] || { echo "blend pie A/B: $tag $arm printed nothing"; exit 1; }
  done
  # Only the lines that carry a pixel statement are compared: the counters move
  # by design (the kernel answers some pixels the chain used to).
  keep='hash|identical|agree|moved|differ|PASS'
  if diff <(grep -iE "$keep" "$out/$tag.off.log") \
          <(grep -iE "$keep" "$out/$tag.on.log") > "$out/$tag.diff"; then
    echo "blend pie A/B $tag: pixel-bearing lines identical"
  else
    echo "blend pie A/B $tag: DIFFERS"; cat "$out/$tag.diff"; exit 1
  fi
}

ab blend-lut tools/kasane_contract/test_blend_lut.c
ab row-table tools/kasane_contract/test_row_table.c
ab coverage  tools/kasane_contract/test_coverage_runs.c
ab visible   tools/kasane_contract/test_visible_skip.c
echo "blend pie A/B PASS: the kernel arm moves no pixel in any of the four corpora"
