# ZENITH lives placement experiment

The sole product change is the life-instance loop in `apps/kasane/proc_megademo.js::patch`, scene4 (ZENITH). Preserve the same position when `(t+i*3)` stays in one four-frame phase; always place at first frame and zoom1 because native `place` also restores visibility. Life2's per-frame `setVisible(on && t<48)` remains. Product b1baab6a040c9f1c6edfbe9242f76df002da01ec, blob19f23d609dd549e304ca7e3529fd745bb13c99d4, directly on main e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267. No native, buffers, workspace/cache lifetime, heap/stack budget, shared CI/frozen reference or PR42/44 edits; no PR61 bounds change mixed in.

## Initial result and adoption gate

Local GCC14 O2 first15 pairs: ZENITH JS_Call ratio0.97386 [0.91816,0.99280], but natural whole-loop JS0.99534 [0.97105,1.01273], JS+present1.00533 [0.97817,1.01540]. Global benefit remains unconfirmed. This initial derived harness still had a dormant audit hook; the cleaned harness has its own binary/hash and independent CI result. Do not conflate these runs. Only host-validation branch publication is intended at this stage; Draft PR/device diagnostic artifacts follow if an overall effect is reproduced. No hardware/USB/flash/merge performed.

## Measurement scope

Same O2/ffp-contract=off binary for both JS inputs; fresh process/context per trial, alternating AB/BA, two discarded pairs, one natural368-frame loop warm then three full loops (1104frames) measured. Actual cached JS_Call(frame) and separate native render/present into the full composed240x135 panel. Pending jobs, end_turn, registration probes, full-panel hash and clock/probe overhead between intervals are excluded from JS+present. It is a sum of these two intervals, not the production owner/scheduler or whole host wall time/FPS. Every run verifies matching scene logs/panels and no exceptions/DEGRADE/present failure. Scene classification uses the original natural lengths and is valid for this no-input timing workload.

One full GC before the measured window then normal automaticGC; no timed heap traversal/logging/per-frame forced GC. The original registration live-count wrappers/memory gate are retained so loader behavior stays representative of the repository fixture; its `pocket.memory.info()` is a common fixed model, not physical ESP heap. Native host cap524288B/initialGC clamp262144, m32 cap163840B/initial81920 (product cap). Threshold can change after collections. Successful allocation/chargedbyte and retained/compile/window peak figures are host TLSF-charge model values, not native ESP allocator measurements. Whole-loop, ZENITH, unrelated NEWS/TWIST/LIMIT and JS+present are reported separately. Never promote a targeted ZENITH result to a full-app FPS claim.

## Long equivalence and compact boundaries

`verify.py` uses the real native renderer; ten natural loops plus one3tier×4phase zoom/tier-rebuild matrix. Actual frame calls/controls trigger scene entry, all four offset phases, zoom1/full44/return0, tier reconstruction during transition and at full-screen, and life2's t48 visibility boundary. One native observer reads fourteen primitive app/loader/input state fields after patch; it does not modify app state. Every final panel is compared directly and each state tuple/log must match. Current native result4964panels/state277984B equal. A short baseline vs mutant run removing only zoom1 restoration keeps all state/log equal but changes final panel388; the negative must be rejected. It is a separate test with no performance figures. No observer is inserted in the timing source. Native/m32 use the same explicit caps as measurement.

```sh
python tools/experiments/zenith-lives/verify.py
python tools/experiments/zenith-lives/bench.py --pairs 15
# Requires gcc-multilib; saved local environment lacks32-bit libc headers:
python tools/experiments/zenith-lives/verify.py --m32
python tools/experiments/zenith-lives/bench.py --m32 --pairs 15
```
