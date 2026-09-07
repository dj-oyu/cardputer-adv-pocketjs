# IMU axis calibration

**Measured on this unit: `X=AX Y=AY Z=AZ`, scale error −0.5%.** The BMI270 is
already aligned with the published frame, so `main/motion.c` keeps the identity
macros. Re-run this if the sensor is reseated or the board revised.

`imucal.js` finds how the BMI270 is oriented on the Cardputer ADV board and
prints the three macros `main/motion.c` needs:

```c
#define MAP_X(ax,ay,az) (...)
#define MAP_Y(ax,ay,az) (...)
#define MAP_Z(ax,ay,az) (...)
```

The orientation is in none of the documents we have — not the schematic, not
M5Unified, not the datasheet — and no amount of software can determine it. The
only way is to hold the device in a known orientation and see which component
gravity lands in. The app walks six of them, two per published axis.

## Why the cable cannot be attached

Two of the six positions cannot be reached with USB plugged in, so the run that
produces the answer is the one run whose log nobody can read. The result is
therefore written to the screen in full and saved through `pocket.storage`;
reopening the app with the cable back on prints it as `IMUCAL_LAST`.

## Why it does not ask for a key press

Two of the six positions put the keyboard against the desk or above the screen,
where Enter is a fight. And a key press is the wrong thing to ask of a hand that
is meant to be holding the device still.

So it records by itself once the reading has been steady for 1.5 seconds, and
answers in sound:

| note | meaning |
| --- | --- |
| high (1046 Hz) | recorded — you may move to the next position |
| low (523 Hz) | it has noticed you moving and is armed for the next one |
| mid (660 Hz) | all six taken, the report is on the USB log |

Re-arming requires real movement. Without that, one position would record six
times over without anyone touching the device.

## Why it distrusts its own answer

Each published axis is observed twice, expecting +g once and −g once. Both
observations must name the same board component with opposite signs. If they
disagree the device was not held as asked, and the app says `IMUCAL_SUSPECT`
rather than printing a mapping that looks authoritative.

It also refuses a mapping where two published axes came from one board axis
(`IMUCAL_DEGENERATE`), which is what a mis-held walk usually produces.

## What it checks about the API itself

This was the first program written against `pocket.*`, and it doubles as that
surface's acceptance test:

- `capabilities.get('sensors.imu')` reports its limits, and asking `watch()` for
  one hertz more than `maxRateHz` must be refused — a published limit the API
  does not enforce is decoration.
- `close()` twice is documented as a no-op. It is called twice.
- The gyroscope runs only while a watch is open, because it draws several times
  the accelerometer's current. `IMUCAL GYRO BEFORE` must read `null`, and
  `IMUCAL GYRO AFTER` — sampled ten frames after the close, because the stop
  reaches the sensor through `motion_poll` and a reading taken immediately would
  say `ON` however well `close()` works — must read `null` too.
- `IMUCAL_GYRO` reports the peak rate seen on each axis. Moving between six
  positions turns the device about every axis, so a flat zero is an axis that is
  not reporting, which the accelerometer walk alone cannot see.

## Why the source is terse

The guest evaluates this file at runtime, so **comments cost heap, not just
flash**. Measured on the device: a 7,228-byte source leaves the guest at 104,244
bytes of its 131,072 limit; at 8,406 bytes it fails to evaluate at all. Linking
Wi-Fi took about 37 KB of DRAM and that headroom went with it. This document
exists so the reasoning does not have to live inside the budget.
