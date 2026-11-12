# Two-part Scene skin QEMU diagnostic

Isolated diagnostic based on public Scene batch candidate 257d2399. Default
apps, original single-mesh skinasset_sim and production manifests are unchanged.

Build with ESP-IDF 6.0.1:

    idf.py -C tools/scenebatch_sim -B build-scenebatch build

No external fixture path. generate.py regenerates the repository-owned public
render_fixture.cjs (plain three-node / two-mesh model), then embeds its bytes
in a bootstrap which calls the actual C optional chunk loader for Scene.
Model SHA256: 24e40884befed04f141904a96faf20c947b2eefd358bb308929c9071aa856132.
Generated/input-hashes.json records model, template, generated bootstrap,
Scene and fixture-generator hashes. The two parts are 6V/2J and 4V/1J; descriptor
IDs are 1 and 2, NOT the single-mesh Blender fixture's ID 0.

120 frames alternate two explicit joint-translation poses every 30 frames.
Each call evaluates BOTH parts with one Scene.evaluateSkins call, then submits
one frame. Acceptance requires complete 32400-pixel transfers, nonempty coverage,
stable per-pose hashes and distinct pose hashes. Last submitted pose survives
double stop + GC + poisoning borrowed arrays and must preserve its pixel hash.
Native skinWorkBytes is queried and printed; it must be positive and <=16384.
Do not hardcode host ABI's 823-byte result on target. Guest memory printed after
stop is not peak. This entry does not exercise OOM (host batch tests do).

Use QEMU serial file capture. QEMU may idle after app_main returns; bounded
runner timeout after SCENEBATCH_DONE PASS is not an application failure.
The native target renderer is scalar for QEMU; no real-device or 20fps claim.
Namespace/admission uses the same test stubs as skinasset_sim. This is NOT the
full production runtime acceptance or independent geometry/image oracle.
No private character, DQ, morph, source-quality or real-device acceptance.
