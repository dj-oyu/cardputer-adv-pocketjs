# BIG WAVE curlAt section window experiment

Base: e0fe98a8c2a3de3b48c8d547c1ff3c8d99863267. Product patch matches shallow
7fd049394f1fa21f25f2ec2b08335441826baef6. Only `curlAt` changes: check the same
strict section interval before the pure `secAmp` calculation. App-owned sections
are dense arrays with numeric `x`/`w`; arbitrary getters/Proxies are not a claim.
There is no new retained object or native buffer/cache/owner change.

Local 64-bit GCC O2 evidence:

- Full scripted lifecycle and three-set bot: 7279 panels / 471679200 RGB565 bytes,
  all native draw records and per-frame state JSON agree. ASan/UBSan also pass.
- Single, overlapping, and empty section fixtures: 21 states and 525 returned
  doubles agree as C hex floats. Calls leave state unchanged. All fixture panels,
  draw records, and final states agree.
- Separate reach probe over 120 actual callbacks: curlAt 1440 -> 1440, secAmp
  3360 -> 480 for title and ride; paused control 0 -> 0. Probe time is not used.
- Without frame oracle wrappers, allocation requests/charged bytes agree:
  title 30663/1683768, ride 31017/1739128. Resident bytes agree; parse peak +24 B.
- 9 paired runs then 15 more, CPU affinity fixed, AB/BA alternating, 120 callbacks
  after 40 title warmup (ride adds start/release). The 15-pair candidate/base JS
  ratios are title .96465 [.93220,.97664], ride .98387 [.96084,1.00049]. Owner plus
  present: title .98544 [.93966,.99227], ride .98856 [.97944,1.02326]. Paused owner
  control 1.01558 [.97722,1.05652]. Intervals are paired-median bootstrap95%, not
  independent device boots. Title improvement remains a host indication; ride
  whole-frame improvement is unresolved.

Owner timing encloses input pump, direct JS_Call, promise drain, end_turn and
Kasane present. The display copies pixels to RAM; this is not product scheduler,
GUEST_ALLOCATOR, LCD/DMA latency or device FPS. Timing excludes full-state and
pixel oracle work and forced per-frame GC. Compatibility runs retain the oracle.

The default host cap is 524288 B. At 163840 B the 64-bit baseline already fails
source evaluation; 16-byte host JSValues are not the device's 8-byte JSValues.
The m32 CI leg uses `-m32 -malign-double` and the product 163840 B cap. It must pass
before claiming device-sized host fit. Local dependency downloads failed with
network 503, not an approval rejection. Neither the product heap/stack budgets
nor guards are raised.

```sh
python3 tools/games/run_bigwave_curl.py --pairs 15 --out .cache/curl-x64
python3 tools/games/run_bigwave_curl.py --sanitize --verify-only --out .cache/curl-asan
M32_CFLAGS='-m32 -malign-double' python3 tools/games/run_bigwave_curl.py \
  --m32 --heap-limit 163840 --pairs 15 --out .cache/curl-m32
```

The last command needs gcc-multilib. Without M32_CFLAGS, the existing repository
rootless m32_sysroot helper is used. Never reuse compiled objects between bitness
or sanitizer modes.

`prepare_bigwave_curl_diag.py` applies the frozen PR50 v2 diagnostic templates from
5bf332368d6e793cd40a8a18a48b4da4ac270622 to a separate clean baseline/candidate
checkout. It uses the product GUEST_ALLOCATOR and owner/scheduler/LCD, 40 natural
title callbacks plus 120 measured callbacks, one app frame per owner turn, and
the unchanged 24-second timeout/watchdog. Default MID tier; no scripted ride or
simulated whole-race FPS claim. INFO boot and product partition/security checks
remain. Logs are capped to 16 app lines plus START/summary/EXIT. The host diagnostic
parity test verifies all 161 panels/draws/states against ordinary execution, plus
six owner completion/deadline cases. Both images carry the same marker overhead.
No device/USB/broker operation is performed by these tools.
