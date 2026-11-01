"""Host study only: a bounded, replayable three-frequency LUT drive.

This is not a firmware API or a performance measurement. Proposed packed core
payload is N*2 bytes for an int16 LUT plus 3*(4+4+4) bytes for uint32 phase/
increment and float32 weight. Reset's retained uint32 seed adds 4 bytes to that
profile. Python tuple/int/float/object overhead is deliberately excluded.
Computation here uses Python's binary64 floats, not a claimed C/JS bit identity.
"""

import math

U32_MASK = (1 << 32) - 1
U32_RANGE = 1 << 32
I16_SCALE = 32767
FREQUENCIES = (.31, .73, 1.13)
WEIGHTS = (.55, .30, .15)


def make_lut(n):
    """Return N periodic int16 sine samples; wrap to index zero at the seam.

    No endpoint is duplicated. Normalize by 32767, so both extrema have equal
    magnitude and interpolation remains bounded without a corrective clamp.
    """
    if type(n) is not int or not 2 <= n <= 65536:
        raise ValueError("LUT size must be an integer in [2, 65536]")
    return tuple(round(I16_SCALE * math.sin(math.tau * i / n)) for i in range(n))


class LutDrive:
    """Three weighted oscillators sharing one LUT; no RNG or sin in step().

    step() samples current phases, then advances them with explicit uint32 wrap.
    Thus holding an instance without calling step() pauses it exactly. Configure
    once; phases are owned mutable state, table/increments/weights are tuples.
    snapshot() is immutable diagnostic state, not a retained live view.
    """

    def __init__(self, size=64, seed=42, step_hz=60):
        if type(seed) is not int or not 0 <= seed <= U32_MASK:
            raise ValueError("seed must be uint32")
        if isinstance(step_hz, bool) or not isinstance(step_hz, (int, float)) or \
                not math.isfinite(step_hz) or step_hz <= 2 * max(FREQUENCIES):
            raise ValueError("step_hz must be finite and exceed twice the highest frequency")
        self.table = make_lut(size)
        self.weights = WEIGHTS
        self.increments = tuple(round(hz * U32_RANGE / step_hz) for hz in FREQUENCIES)
        # Seed affects initial phases only. LCG arithmetic is explicitly uint32,
        # including seed zero; no random state is advanced during simulation.
        self.seed = seed
        self.phases = [0, 0, 0]
        self.reset()

    def step(self):
        value, size = 0., len(self.table)
        for k in range(3):
            scaled = self.phases[k] * size
            index, fraction = scaled >> 32, (scaled & U32_MASK) / U32_RANGE
            a, b = self.table[index], self.table[(index + 1) % size]
            value += self.weights[k] * (a + (b - a) * fraction) / I16_SCALE
            self.phases[k] = (self.phases[k] + self.increments[k]) & U32_MASK
        return value

    def reset(self):
        """Restore initial phases in place; setup parameters remain unchanged."""
        state = self.seed
        for k in range(3):
            state = (1664525 * state + 1013904223) & U32_MASK
            self.phases[k] = state

    def snapshot(self):
        return tuple(self.phases), self.increments, self.weights
