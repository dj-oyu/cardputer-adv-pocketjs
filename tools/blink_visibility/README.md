# Visibility-only host acceptance

Public disjoint red/blue synthetic rectangles. Uses existing Scene.handle.visible,
evaluate and submit; no product source changes. `python3 tools/blink_visibility/run.py`.
343 binary64 control combinations including .97999999 and .98, then reverse order,
compare to four native-rendered visibility states. The C expectation independently
uses both<threshold AND local<threshold. Pixel checks enforce non-overlap, left/right
regions and composition, not an independent raster/shading oracle. Checks IO retry,
invalid controls before mutation, and submitted pixels surviving subsequent visibility
mutation/disposal/GC. Existing namespaces are host stubs, not product admission.
This is NOT mask/morph/DQ, arbitrary geometry, private material or quality equivalence,
OOM acceptance or real-device/20fps evidence. ASan/UBSan enabled, LSan off, ledger0.
The rejected loader runner and its files are not used or changed.
