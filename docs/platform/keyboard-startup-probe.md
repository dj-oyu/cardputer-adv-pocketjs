# Keyboard startup probe recovery

Base: main `6ca8058dd5756790211d4baff73c35268a0bdbf1`. This is independent
of PR57's result HUD optimization. Its baseline and candidate both encountered
an abort before the guest/UI task started. Exact-ELF disassembly identified the
return from `board_init()`; the old code converted every unsuccessful probe
into `ESP_ERR_NOT_FOUND`. The original error was not saved, so the physical
cause is still unknown. A later successful boot does not diagnose that cause.

## API and device evidence

- [ESP-IDF v6.0.1 probe documentation](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32s3/api-reference/peripherals/i2c.html#_CPPv416i2c_master_probe23i2c_master_bus_handle_t8uint16_ti)
  distinguishes a NACK/device-not-found from a timeout. It cautions that a
  timeout can indicate a bus/pull-up problem; it is not proof of an absent IC.
- [The pinned IDF implementation](https://github.com/espressif/esp-idf/blob/v6.0.1/components/esp_driver_i2c/i2c_master.c#L1255)
  also returns timeout on bus-lock acquisition failure. Probe uses **100 kHz**
  independently of the keyboard device's subsequent 400 kHz transfers, and
  resets its transaction status before returning. Its internal lock, command
  and event waits mean the timeout argument is not a hard whole-call deadline.
- [TI TCA8418 Rev.G](https://www.ti.com/lit/gpn/TCA8418), sections6.7/8.4/10,
  describes reset recovery (120us minimum) and power-on reset. An MCU software
  reset is not evidence that the keyboard power/reset sequence occurred.
  No reset pin, power rail, pull-up, clock or bus configuration is changed here.

## Bounded policy, not a root-cause claim

`keyboard_probe_startup()` requires an actual ACK at0x34. It retains the existing
50ms IDF argument and allows at most3 calls, retrying only NOT_FOUND or TIMEOUT.
Other errors stop immediately. Retry gaps are10ms (rounded to at least one RTOS
tick), comfortably longer than the device reset-recovery minimum without
assuming that a reset occurred. Three attempts and a250ms retry-start window
are deliberately small **firmware policy choices**, not TI-mandated values or
a measured cure. A second/third call is suppressed when its immediately preceding
clock check observes250ms elapsed. The window is checked before and after each
gap. Preemption after that check can delay actual call entry. The code cannot
cancel an in-flight IDF call, logging or RTOS preemption;250ms is **not** a bound on
the function's elapsed return time. Attempt count and each IDF timeout remain
finite. A late positive ACK is still a real success; no error becomes success.

Failure logs preserve every original error name/number, attempt, elapsed time
and instantaneous SDA8/SCL9 levels. TIMEOUT alone still cannot distinguish a
lock wait from a transaction timeout; retain the preceding IDF driver logs.
Line levels are observations, not a waveform or a power diagnosis. Recovery
logs the successful attempt count. Permanent failure returns the last original
error to the existing fatal `ESP_ERROR_CHECK(board_init())`; it does not boot a
keyboard-less fallback. Only probe is retried, never a partially completed
keyboard register setup. No allocation, task or persistent retry state is added.
At most4 wrapper log lines are emitted per boot; existing IDF logs are separate.

## Verification and separately authorized device work

Run `python3 tools/board_test/run_keyboard_probe.py` and the same command with
`--sanitize`. The actual helper is linked against deterministic SDK fakes, at
100 and1000Hz tick rates. One table covers normal ACK, mixed transient errors,
persistent NACK/timeout, nonretryable errors and the exact/one-us-before retry
window boundaries. It checks original return values/log codes/pins, max3 calls,
no delay after terminal failure and no retry after sampled window expiry.
The actual keyboard-configuration tail from board.c is compiled too: recovery
configures once, while a post-ACK write error stays fatal with no configured
message/retry. A successful SDK call taking60ms is not rejected as though50ms
were a whole-call deadline.
The dedicated CI also builds the normal ESP32-S3 product with IDF6.0.1; no
automatic performance diagnostic or guest/stack/watchdog change is included.

**Device efficacy remains untested.** Do not flash, reset or restore merely
because this document or a CI artifact exists. Once a specific head and those
actions are separately approved, use the registered-source/build-receipt broker
path and its normal boot/readback gates. Do not use direct USB or idf.py to bypass
it. Verify the actual build's source/ELF/version and normal configuration.

For each pre-agreed finite boot observation, preserve complete startup logs:
`KBD_PROBE` attempt/error/elapsed/SDA/SCL, all preceding i2c.master lines, the
ready or failed summary, `ADV keyboard detected`, and `HOME_READY` if reached.
Retain failed/recovered/first-attempt successes, not just the successful boots.
On a successful boot, a separately authorized physical key press/release checks
the real keyboard path; injected text alone cannot prove that hardware path.
A persistent failure or another truncated/multiple-startup capture stops the
observation. No repeat-until-success, guard relaxation or performance claim.
Even recovery only shows that the bounded policy helped that boot; use the saved
original error/line-level evidence to decide whether electrical/timing work is
still necessary. PR57's incomplete ABBA stays separate and unmerged.
