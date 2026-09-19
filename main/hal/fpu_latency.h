#pragma once
// Measures this part's dependent-issue latency for the scalar FPU and the
// FPU<->general-register crossings, and logs it. Owner task, no allocation,
// about 0.1 s. See fpu_latency.c for why it exists.
void fpu_latency_run(void);
