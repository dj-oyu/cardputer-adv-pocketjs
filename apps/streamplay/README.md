# streamplay

Plays `assets:/chime.wav` through `pocket.audio.player` and reports what feeding
it cost the frame. It exists because streamed playback could be reasoned about
but not heard or timed: nothing else in the tree opens a player.

## What it measures, and why in two phases

The claim under test is that filling the ring costs the drawing task under a
millisecond a frame. The producer is `player_feed()` on that same task, so the
cost is a difference between frames that are feeding and frames that are not —
which means both have to be measured on the same board in the same run. So the
app holds a player **open** for 60 frames without playing (the ring is not even
allocated yet), then plays, and reports the two means.

`Date.now()` is whole milliseconds, far coarser than the difference being looked
for. That is why nothing here looks at a single frame: 60 frames at 1 ms
resolution puts a mean inside about 0.02 ms, which is finer than the 1 ms the
prediction is about. `worstFrameMs` is reported separately and is the one number
here that *is* a single frame — read it as an upper bound, not as a typical cost.

## Why it cycles instead of waiting for a key

The lead asked for a keypress trigger. There is not one to have: `input.onKey`
throws `UNSUPPORTED` on this host (`pocket_ui.c`), because there is no channel
from the keyboard to a guest, which is why no app in the tree uses it.

Cycling turns out to be the better instrument anyway. Each cycle is 60 idle
frames then one full play, so the log carries **repeated paired windows on one
binary at runtime** — which is the comparison being asked for, taken several
times rather than once, with no two builds and nothing to press. It also means
the click train repeats on its own while somebody listens for gaps.

An `ended` player is not replayable by contract, so a repeat is a fresh `open()`.
That is not a workaround: it is what makes each cycle an independent measurement
rather than a continuation of the last one.

## The line that answers it

What it actually reported, on the board, 2026-09-08 — this is a real run and
not an illustration, and the shape of it is the result:

```
cycle=4 .. 13   underruns=0   deltaTenthMs -2..-4   worstFrameMs 35..36
                idleTenthMs 337-338   playTenthMs 334-335
```

Thirteen consecutive cycles with nothing moving. `deltaTenthMs` came out
slightly **negative**, which is noise and not a saving: the honest reading is
that **feeding costs less than this instrument can resolve**, against a
predicted bound of 1 ms. Do not quote -0.3 ms as a speed-up.

The markers, which the USB scripts may treat as a contract and which will not
change without being said so:

```
STREAM CAP <supported> <available>
STREAM OPEN codec=.. rate=.. durationMs=.. seekable=..    (first cycle only)
STREAM STATE <state> [error=<CODE>]
STREAM T pos=<ms> underruns=<n> state=<s>                 (every 15 frames)
STREAM DONE cycle=.. state=.. underruns=.. idleFrames=.. idleTenthMs=..
            playFrames=.. playTenthMs=.. deltaTenthMs=.. worstFrameMs=..
STREAM_FAIL <code>
```

`deltaTenthMs` is the cost of feeding, in tenths of a millisecond — 6 is 0.6 ms.
`underruns` counts 128-frame blocks the audio task had to fill with silence
because the pump was late; each is 5.3 ms of inserted silence and **no audio is
lost**, so a nonzero count means the clip got longer, not that part went missing.

How to read a future result:

- `underruns=0` and a small `deltaTenthMs` — the design holds.
- **`deltaTenthMs` growing from cycle to cycle** — something is leaking across
  the player's open/close, which one cycle could never have shown. This is the
  reason the app repeats rather than reporting once and stopping.
- `underruns` nonzero at rest — the ring is too small for this renderer's
  measured 39.9 ms frames. The fix is a fourth slot in `sound_stream.h`, not a
  faster read.
- `deltaTenthMs` above about 20 — the read is dearer than estimated and the feed
  belongs on a task of its own at priority 4, which would first need the `fs`
  surface made thread-safe.

## What it cannot tell you

Whether it sounded right. `board_capture` copies the framebuffer, not what
reached the ES8311, so no software on this board can hear itself. The clip is a
click train precisely because a gap is obvious in one and easy to miss in a
chord — that check is a person with the speaker, and it is the only one.

The clip itself is `apps/chime/README.md`; the mechanism is
`docs/common-api.md` §9.1.1 and `main/hal/sound_stream.h`.
