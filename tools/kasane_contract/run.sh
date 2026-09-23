#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -DKSN_CONTRACT_HOST -Imain/ui/kasane \
  tools/kasane_contract/use_cases.c tools/kasane_contract/probe.c -o "$out/probe"
"$out/probe"
cc -std=gnu11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -Imain/ui/kasane -Itools/kasane_contract main/ui/kasane/ksn_core.c \
  tools/kasane_contract/use_cases.c tools/kasane_contract/test_core.c -o "$out/core" main/ui/kasane/ksn_blend_pie.c
"$out/core"
for options in '-g -fsanitize=address,undefined' '-O2 -fstrict-aliasing'; do
  cc -std=c11 -Wall -Wextra -Werror $options -Itools/kasane_contract/p0_hostshim -Itools/hostshim \
    tools/kasane_contract/test_p0_histogram.c -o "$out/p0-histogram"
  "$out/p0-histogram"
  cc -std=c11 -Wall -Wextra -Werror $options -DP0_TIMING_ONLY \
    -Itools/kasane_contract/p0_hostshim -Itools/hostshim \
    tools/kasane_contract/test_p0_histogram.c -o "$out/p0-timing-only"
  "$out/p0-timing-only"
  cc -std=c11 -Wall -Wextra -Werror $options -DP0_TIMING_ONLY -DKASANE_P0_BUS_PROBE \
    -Itools/kasane_contract/p0_hostshim -Itools/hostshim \
    tools/kasane_contract/test_p0_histogram.c -o "$out/p0-bus"
  "$out/p0-bus"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane -Imain/pocket \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c main/pocket/app_notice.c main/ui/kasane/ksn_indicator.c \
    tools/kasane_contract/test_notice.c -o "$out/notice"
  "$out/notice"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c tools/kasane_contract/test_runtime.c -o "$out/runtime"
  "$out/runtime"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c tools/kasane_contract/test_app_teardown.c -o "$out/teardown"
  "$out/teardown"
  for fill_mode in '' '-DKSN_PIE_FILL_MODEL'; do
    cc -std=c11 -Wall -Wextra -Werror $options $fill_mode -Imain/ui/kasane \
      main/ui/kasane/ksn_core.c tools/kasane_contract/test_fill.c -o "$out/fill" main/ui/kasane/ksn_blend_pie.c
    "$out/fill"
  done
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c tools/kasane_contract/test_view.c -o "$out/view"
  "$out/view"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain -Imain/ui/kasane -Imain/pocket -Itools/kasane_contract \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c main/pocket/app_legacy_presenter.c \
    tools/kasane_contract/test_presenter.c -o "$out/presenter"
  "$out/presenter"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane -Itools/kasane_contract \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c main/ui/kasane/ksn_schema.c \
    tools/kasane_contract/test_schema.c -o "$out/schema"
  "$out/schema"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane -Itools/kasane_contract \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c main/ui/kasane/ksn_schema.c \
    main/ui/kasane/ksn_schema_session.c tools/kasane_contract/test_schema_session.c -o "$out/schema-session"
  "$out/schema-session"
  cc -std=c11 -Wall -Wextra -Werror $options -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -Imain/ui/kasane main/ui/kasane/ksn_schema.c \
    main/ui/kasane/ksn_source.c tools/kasane_contract/test_source.c -o "$out/source"
  "$out/source"
  cc -std=c11 -Wall -Wextra -Werror $options -pthread -Imain/ui/kasane \
    main/ui/kasane/ksn_source_pool.c tools/kasane_contract/test_source_pool.c \
    -o "$out/source-pool"
  "$out/source-pool"
  cc -std=c11 -Wall -Wextra -Werror $options -pthread -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -Imain/ui/kasane main/ui/kasane/ksn_schema.c \
    main/ui/kasane/ksn_source.c main/ui/kasane/ksn_source_pool.c \
    main/ui/kasane/ksn_source_pool_adapter.c \
    tools/kasane_contract/test_source_pool_adapter.c -o "$out/source-pool-adapter"
  "$out/source-pool-adapter"
  cc -std=c11 -Wall -Wextra -Werror $options -DKSN_SCHEMA_DIRTY_COUNT \
    -Imain/ui/kasane -Itools/kasane_contract \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c main/ui/kasane/ksn_schema.c \
    main/ui/kasane/ksn_schema_session.c tools/kasane_contract/test_schema_workloads.c -o "$out/schema-workloads"
  "$out/schema-workloads"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/ui/kasane/ksn_cache.c \
    main/ui/kasane/ksn_modal.c main/ui/kasane/ksn_view.c tools/kasane_contract/test_repair.c -o "$out/repair"
  "$out/repair"
  cc -std=c11 -Wall -Wextra -Werror $options -DKSN_FROST_PIE_MODEL -Imain/ui/kasane \
    main/ui/kasane/ksn_frost.c tools/kasane_contract/frost_baseline.c tools/kasane_contract/test_frost_equivalence.c -o "$out/vector" main/ui/kasane/ksn_blend_pie.c
  "$out/vector"
  cc -std=c11 -Wall -Wextra -Werror $options \
    -Imain/ui/kasane main/ui/kasane/ksn_core.c tools/kasane_contract/test_review.c -o "$out/review" main/ui/kasane/ksn_blend_pie.c
  "$out/review"
  cc -std=c11 -Wall -Wextra -Werror $options \
    -Imain/ui/kasane tools/kasane_contract/test_exhaustion.c -o "$out/exhaustion" main/ui/kasane/ksn_blend_pie.c
  "$out/exhaustion"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c tools/kasane_contract/test_resources.c -o "$out/resources" main/ui/kasane/ksn_blend_pie.c
  "$out/resources"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c tools/kasane_contract/test_render.c -o "$out/render"
  "$out/render"
  # The narrow arm: a port that offers present_rect gets only the damaged
  # columns, and the panel it produces has to equal the full-width arm's.
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane     main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c tools/kasane_contract/test_narrow.c -o "$out/narrow"
  "$out/narrow"
  # Boundary 7: the render path's phase counts, and the pixel identity of the
  # two arms inside one binary -- the switch off must not move a pixel, and the
  # counts must not move with it off.
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c tools/kasane_contract/test_render_prof.c -o "$out/render-prof"
  "$out/render-prof"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c tools/kasane_contract/test_primitives.c -o "$out/primitives"
  "$out/primitives"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c tools/kasane_contract/test_text_render.c -o "$out/text"
  "$out/text"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c tools/kasane_contract/test_image_render.c -lm -o "$out/image"
  "$out/image"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c tools/kasane_contract/test_image_rotate_arms.c -o "$out/image-rotate-arms"
  "$out/image-rotate-arms"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c tools/kasane_contract/test_image_stretch_arms.c -o "$out/image-stretch-arms"
  "$out/image-stretch-arms"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c tools/kasane_contract/test_animation.c -o "$out/animation"
  "$out/animation"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane -Imain/pet \
    main/pet/ksn_pet.c main/pet/pet_pixels.c tools/kasane_contract/test_pet_provider.c -o "$out/pet-provider" main/ui/kasane/ksn_blend_pie.c
  "$out/pet-provider"
  # Both arms of the PET provider's decoded-row cache in one binary. The
  # counters exist only under -DKSN_PET_ROW_STATS (the shipping object has
  # none) and the harness needs the core and the renderer because it mounts and
  # renders real scenes. docs/perf/kasane-pet-row-cache.md 7 asks for exactly
  # this line: it was left out of the provider commit so the wiring is its own
  # change.
  cc -std=c11 -Wall -Wextra -Werror $options -DKSN_PET_ROW_STATS -Imain/ui/kasane -Imain/pet \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/pet/ksn_pet.c main/pet/pet_pixels.c \
    tools/kasane_contract/test_pet_row_cache_arms.c -o "$out/pet-row-cache"
  "$out/pet-row-cache"
  python3 tools/make_font.py "$out"
  cc -std=c11 -Wall -Wextra -Werror $options -Itools/kasane_contract/fontshim \
    -Itools/hostshim -Imain/hal -Imain/text -Imain/ui/kasane -I"$out" \
    main/text/ksn_font.c tools/kasane_contract/test_font.c -o "$out/font" main/ui/kasane/ksn_blend_pie.c
  "$out/font"
  cc -std=c11 -Wall -Wextra -Werror $options -DKSN_SPAN_COUNT -Itools/kasane_contract/fontshim \
    -Itools/hostshim -Imain/hal -Imain/text -Imain/ui/kasane -I"$out" \
    main/text/ksn_font.c tools/kasane_contract/test_span_count.c -o "$out/span-count"
  "$out/span-count"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c tools/kasane_contract/test_group_dither.c -o "$out/group-dither"
  "$out/group-dither"
  # One script, one binary, three arms of g_ksn_decode_once; ksn_core_read is
  # wrapped so the read counts are counted call sites, not estimates.
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c \
    tools/kasane_contract/test_decode_reuse.c -Wl,--wrap=ksn_core_read -o "$out/decode-reuse"
  "$out/decode-reuse"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/ui/kasane/ksn_cache.c \
    tools/kasane_contract/test_cache.c -o "$out/cache"
  "$out/cache"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c tools/kasane_contract/test_cache_exhaustion.c main/ui/kasane/ksn_blend_pie.c \
    -o "$out/cache-exhaustion"
  "$out/cache-exhaustion"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
    tools/kasane_contract/test_coverage_runs.c -o "$out/coverage-runs" main/ui/kasane/ksn_blend_pie.c
  "$out/coverage-runs"
  # Candidate 4c: the row profile. The test includes ksn_render.c -- the table
  # and `sample` it is compared against are statics there -- so only the core and
  # the cache are linked beside it.
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
    tools/kasane_contract/test_row_table.c -o "$out/row-table" main/ui/kasane/ksn_blend_pie.c
  "$out/row-table"
  # Boundary 4's quantized-key table. Same shape as the row table above: the
  # test includes ksn_render.c (the table and the chain it is compared against
  # are statics there), so only the core and the cache are linked beside it.
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
    tools/kasane_contract/test_blend_lut.c -o "$out/blend-lut" main/ui/kasane/ksn_blend_pie.c
  "$out/blend-lut"
  # The same table with the coarse 255 -> 256 arm forced on (KSN_SCALE256_ARM=1,
  # see docs/perf/kasane-alpha256.md): a row has to be built from the arm that is
  # running, so the solid arm must still move nothing. Without the switch in
  # blend_lut_build_row this arm reports the coarse chain's own one-step error.
  cc -std=c11 -Wall -Wextra -Werror $options -DKSN_SCALE256_ARM=1 -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
    tools/kasane_contract/test_blend_lut.c -o "$out/blend-lut-coarse" main/ui/kasane/ksn_blend_pie.c
  "$out/blend-lut-coarse"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c main/ui/kasane/ksn_render.c main/ui/kasane/ksn_blend_pie.c main/ui/kasane/ksn_modal.c \
    tools/kasane_contract/test_composition.c -o "$out/composition"
  "$out/composition"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_frost.c tools/kasane_contract/frost_baseline.c tools/kasane_contract/test_frost_equivalence.c -o "$out/equivalence" main/ui/kasane/ksn_blend_pie.c
  "$out/equivalence"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_frost.c tools/kasane_contract/test_frost.c -o "$out/frost" main/ui/kasane/ksn_blend_pie.c
  "$out/frost" > "$out/frost.bin"
  python3 tools/kasane_contract/frost_reference.py "$out/frost.bin"
  cc -std=c11 -Wall -Wextra -Werror $options -Imain/ui/kasane \
    main/ui/kasane/ksn_frost.c tools/kasane_contract/test_stress.c -o "$out/stress" main/ui/kasane/ksn_blend_pie.c
  "$out/stress" > "$out/stress.bin"
  python3 tools/kasane_contract/stress_reference.py "$out/stress.bin"
done
python3 tools/pie/test_frost.py
python3 tools/kasane_contract/fill_pie.py
# Boundary 4a's coarse 255 -> 256 arm (g_ksn_scale256, default 0 = the exact
# path). The model flips the switch inside one binary and renders 120 frames
# through the real ksn_render_rects, so the moved pixels and the worst step are
# measured rather than asserted; the exact arm is checked against the pre-change
# reference over the whole domain (0 differences).
python3 tools/pie/run_models.py scale256
# Measured, not estimated: -finstrument-functions counts entries into the pixel
# predicate and into the row solver, so this arm prints the before/after call
# counts per frame (see the reporter in test_coverage_runs.c).
cc -std=c11 -Wall -Wextra -Werror -O2 -fno-inline -finstrument-functions -DKSN_COUNT_CALLS \
  -Imain/ui/kasane main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
  tools/kasane_contract/test_coverage_runs.c -o "$out/coverage-count" main/ui/kasane/ksn_blend_pie.c
"$out/coverage-count"
# The same arm for the row table: entries into `sample` per frame and into the
# builder, so "the per-pixel sampling became a load" is a count of calls.
cc -std=c11 -Wall -Wextra -Werror -O2 -fno-inline -finstrument-functions -DKSN_COUNT_CALLS \
  -Imain/ui/kasane main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
  tools/kasane_contract/test_row_table.c -o "$out/row-table-count" main/ui/kasane/ksn_blend_pie.c
"$out/row-table-count"
# Boundary 4e's measurement: the visible threshold skip, counted before it is
# implemented (docs/perf/kasane-opt-survey.md). Two builds of the same harness --
# with and without the counters -- must print the same scene hashes, so "the
# counters change no pixel" is a diff rather than a claim. The harness turns the
# blend table off: what it counts is the chain's own pixels, which is the
# population a threshold skip would be judged on (see the harness header).
cc -std=c11 -Wall -Wextra -Werror -O2 -Imain/ui/kasane -Itools/kasane_contract \
  main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
  tools/kasane_contract/test_visible_skip.c -o "$out/visible" main/ui/kasane/ksn_blend_pie.c
"$out/visible" > "$out/visible.plain"
cc -std=c11 -Wall -Wextra -Werror -O2 -DKSN_COUNT_VISIBLE -Imain/ui/kasane -Itools/kasane_contract \
  main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
  tools/kasane_contract/test_visible_skip.c -o "$out/visible-count" main/ui/kasane/ksn_blend_pie.c
"$out/visible-count" > "$out/visible.count"
diff <(grep '^scene' "$out/visible.plain") <(grep '^scene' "$out/visible.count")
grep '^CORPUS' "$out/visible.count"
echo "visible skip PASS: the counters change no pixel (5 scene hashes identical)"
# The same arm for the blend LUT: entries into the per-pixel chain and into the
# table read, so "the chain became a row lookup" is a count of calls.
cc -std=c11 -Wall -Wextra -Werror -O2 -fno-inline -finstrument-functions -DKSN_COUNT_LUT \
  -Imain/ui/kasane main/ui/kasane/ksn_core.c main/ui/kasane/ksn_cache.c \
  tools/kasane_contract/test_blend_lut.c -o "$out/blend-lut-count" main/ui/kasane/ksn_blend_pie.c
"$out/blend-lut-count"
printf '#include "ksn_api.h"\n#include "ksn_ports.h"\n#include "ksn_core.h"\n#include "ksn_cache.h"\n#include "ksn_modal.h"\n#include "ksn_view_host.h"\n#include "ksn_runtime.h"\n' | \
  c++ -std=c++17 -Wall -Wextra -Werror -Imain/ui/kasane -x c++ -fsyntax-only -
