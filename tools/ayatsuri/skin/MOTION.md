# Public synthetic Motion → native skin integration

Test-only continuation from reviewed base `0047ada8a987044a86d60b46ae793b5be5fe72e3`.
No product Scene API, importer, wire format, model assets or defaults change.

```sh
python3 tools/ayatsuri/skin/run_motion.py --no-leak-check
```

Use `--no-leak-check` only where ptrace prevents LeakSanitizer. ASan/UBSan and
requested-allocation ledgers remain enabled. The real production QuickJS and
`apps/lib/ayatsuri_motion.js` are linked/loaded, not a JS mock or Node renderer.

A test-only app adapter maps frame IDs to explicit milliseconds, evaluates three
additive sine-wave channels, checks them against analytic expectations, and feeds
`pocket.ayatsuri.compose` mesh-local joint globals into existing `evaluateSkin`.
The public fixture has six vertices and two joints. Its inverse bind offset and
existing immutable native Model remain unchanged. Keys are constant: this is
wave/channel integration, not new coverage of arbitrary key interpolation.

The existing skin harness then checks 240 forward/reverse visits, exact images
against its native reference, independent actors, invalid input and OOM recovery,
source disposal before pending-frame rendering, IO retry and cyclic GC release.
The added hook repeats one explicit time 24 times and verifies identical images,
zero linked warm evaluation allocations, and NaN/Infinity time rejection without
changing globals or the last published Motion result. Four conditions cover
4/8 strip rows × row/span renderer selection. `evaluateSkins` batch is not added
or newly tested by this slice.

The image reference shares the native skin/render core, so this proves app-to-
native integration and lifetime contracts, not independent skin mathematics,
shader correctness, target RAM/FPS or Blender/fox quality. The existing Scene
facade still has no automatic skin control; code never accesses its internal
instance to pretend otherwise. Native library registration is the existing host
harness, not a claim about product lazy-loading memory.

Logs and `motion-inputs.json` are in `.cache/motion-skin-host/`; input hashes are
checked unchanged across the run, alongside compiler/source evidence in
`commands.json`. Logs record allocation counts and guest-frame sample peaks,
not whole-process peak memory. LSan absence is explicitly recorded.
