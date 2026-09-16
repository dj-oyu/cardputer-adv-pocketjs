#pragma once
// Fixed-point sine, cosine and tangent.
//
// The three libm routines these stand in for are not in the image for their own
// sake: on this part a C call to sinf becomes a soft-float thunk into the Rust
// core's compiler_builtins, where sinf is a 2,470-byte routine, cosf 2,394 and
// tanf 1,925. The scenes evaluate them per vertex and per frame, and the MP3
// decoder's filter once per file, so dropping the last C reference is the whole
// of the change and --gc-sections drops the rest.
//
// Q31, with a 65-entry cosine table covering one quadrant and a degree-5
// correction on the table point. Measured against the true value it is 0.27
// ulp(1) out at worst over the sweeps in tools/test_fxmath.c, and differs from
// this part's libm sinf in the last bit on about 3.7% of arguments.
//
// Accurate for |x| up to about 10^6 radians, which is a day of any scene clock;
// beyond that it keeps returning something in [-1,1] rather than failing, but
// the reduction loses meaning. Anything non-finite reads as an angle of zero.
float fx_sinf(float x);
float fx_cosf(float x);
// sin/cos in float, and only scene/wave.c asks: the horizon's tilt.
float fx_tanf(float x);
