# Host cleanup boundary probe
Run: `python3 run.py <firmware-checkout>`.
Extracts actual app_stop(), compiled with probe macros disabled and deterministic fake owners/guest. Links actual shiki_heap.c. No product edits.
Checks partial startup (no guest), guest-finalizer release, outstanding owner closing/restart refusal/lastfree/restart, repeated stop.
NOT full app_start/manifest admission, real owner reset functions, real QuickJS finalizers, target RAM/stack, transport or device FPS acceptance. No leak-sanitizer claim.
Initial harness incorrectly expected active=true during closing; source defines active as region&&!closing. Fixed harness expectation, not product source. Both optimized and ASan/UBSan then PASS.
Source checkout at run: 6ad3b446f4e31ab96fea2dbaa176d0966a056e3b; heap Git blob244d87adf29fdba171a76a4c2cf03bd7cde49d50 matches public257d2399. No whole-head equivalence claim.

The tested app_session.c and shiki_heap.c are byte-identical between local 6ad3b446 and public parent e0db3e09ac7a572eac1052cdac2b9a1dbb452e3e (git diff restricted to those files is empty). Extracted range: `void app_stop(void) {` through the following resident-suspension section marker. Optional probe macros are not defined.

Separate `heap_lifecycle.c` checks single-block closing/restart without session stubs. Compile with `cc -std=c11 -Wall -Wextra -Werror -O2 -I <repo>/main/pocket heap_lifecycle.c <repo>/main/pocket/shiki_heap.c -o <temporary-output>`; asserts must remain enabled.

Review: independent source review found no major issue after adding explicit success assertions for the finalizer and delayed-owner allocations. The fake Kasane reset always succeeds; pending transport/reset failure paths are NOT covered. Real guest finalizers are NOT exercised. Final test SHA256 is recorded in results.log. Source hashes were rechecked after the run; no product source changed during this isolated probe.
