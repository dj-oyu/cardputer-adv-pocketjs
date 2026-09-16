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
// The implementation is a Q31 table of one quadrant of the cosine (218 entries,
// 872 bytes, tools/gen_fx_lut.py's output in fx_lut_gen.h) with a three-point
// Lagrange through the entry either side of the angle. Measured over the sweeps
// in tools/test_fxmath.c it is 0.97 ulp(1) from the true value at worst -- the
// floor set by the table's own half-LSB rounding, and reached at 218 entries
// where a linear interpolation would need 2,275 (9,104 bytes) for 0.99 ulp. It
// differs from this part's libm in the last bit on about 48% of arguments, of
// which the visible effect is: 14 pixels of the 84,661,200 in the scene dump
// harnesses (flower 5, solar 9, stars and glass_rain none), every one of them a
// single shading step. The degree-5 polynomial this replaced ran at 0.27 ulp and
// moved 45; both move the same few last-bit differences, which is why the
// cheaper table was taken.
//
// Accurate for |x| up to about 10^6 radians, which is a day of any scene clock;
// beyond that it keeps returning something in [-1,1] rather than failing, but
// the reduction loses meaning. Anything non-finite reads as an angle of zero.
float fx_sinf(float x);
float fx_cosf(float x);
// sin/cos in float, and only scene/wave.c asks: the horizon's tilt.
float fx_tanf(float x);
