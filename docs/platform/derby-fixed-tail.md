# Finite fixture tail and watchdog (2026-10-19)

The physical baseline at source `dd0b480967bfe1bc08198b6e77baf3a98356735a`
passed image readback/boot matching and emitted READY. Its first post-window
tail failed before publishing a repetition: `InternalError: interrupted` at
`step (user.js:59)` -> `settle (derby_scene.js:99)` -> generated
`derby_play.js:171`. The private raw receipt is 29,723 bytes, SHA-256
`c563f2f38f86c2782caffc8f2d9b89e0387d2b3f76a658aae6859a716c3b74d1`.
STOP/HOME preceded `END reps=0 valid=0 quality=2`. Quality 2 is DF_STATE;
the owner detects the stopped app. Zero summaries does not mean the measured
120 contexts were never reached: publication follows the tail/finish check.

The old fixture calls `settle()` immediately after step 510. Its original
`while (rs.done < 8 && rs.t < 200) step(F, rs, DT)` executes the remaining
735 steps in one logical frame for this seed. The ordinary game calls it only
after three horses finish. The session watchdog in `main/app_session.c`
(`interrupt`, `arm_turn`) rejects a stop request or a turn exceeding 250 ms;
`guest_interrupt` in `components/pocketjs_guest/src/guest.c` delegates to it.
`drain_runaway` additionally limits accumulated logical-frame work, so a VM
continuation alone does not solve an oversized logical frame. The observed
stack identifies this synchronous tail; the raw log does not distinguish
the watchdog's deadline and stop-request predicates individually.

The generated fixture now executes at most eight of the same simulation steps
per logical frame and actually returns to the owner while work remains.
After the unchanged terminal predicate becomes false, the original `settle()`
runs once for final settlement. The seed, DT=.05, step order, terminal
predicate, finish digest and 390->510 measurement window stay identical.
The tail never calls the original frame or paints. A 512-tail-turn bound
also fails closed; the fixture's 180-second session and all cancellation,
watchdog, transport and quality guards retain their existing limits.

Host coverage runs the actual generated app/native fixture and verifies three
735-step tails, 92 returned turns each, at most eight steps per turn, one
settlement, zero guest tail draws and the original finish digest. Each measured
window still requires the known 120 contexts, 4,975 draws, work counts, draw
input and pixel hashes. A pure oracle executes the actual generated simulation
with the previous synchronous loop to compare work/result. The host harness
now arms the 250ms backstop on frame calls; previously its only deadline was
cleared after initialization. A separate deterministic 32-step stress interrupt
also rejects a copy of the old synchronous fixture through QuickJS's fatal
`InternalError: interrupted` path on fast hosts. This stress model is not S3
timing proof. Existing collector negatives remain, including STOP/HOME before
an invalid END. Physical retry requires review of the new heads.
