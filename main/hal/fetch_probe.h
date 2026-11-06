#pragma once
// Instruction-fetch probe (CONFIG_POCKET_FETCH_PROBE, measurement only): times
// the same straight-line and one-miss-per-line code from IRAM, RTC fast
// memory and flash, to price RTC fast memory as a code tier between the two.
void fetch_probe_run(void);
