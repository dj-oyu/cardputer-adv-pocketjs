# P3: seekable audio source on Cardputer (2026-09-24)

## Scope and setup

The `j` USB diagnostic mounts one text node bound to `pocket.audio.outputSource()`.
It creates a unique, silent 24 kHz IMA-ADPCM WAV in `app:/` (11 × 2048-byte
blocks, 22,576 bytes, 1,874 ms), plays it, seeks to 1,120 ms after roughly
200 ms of playback, waits for position to advance past 1,200 ms, and lets it
end. The file is removed after closing the player. The probe refuses to
overwrite an existing file. It neither requests SD access nor modifies a
user track. Each filesystem write is at most 1,024 bytes, per the public API.

Final diagnostic image: `build_kasane_p3_copy/cardputer_pocketjs.bin`,
`KASANE_P0_PROBE=ON`, `KASANE_P0_COPY_PROBE=ON`. Final run:
`.cache/kasane-p3-seek-device-20260924/j9/summary.json` and `serial.log`.
Command: `tools/kasane_output_source_device.py --port COM3 --probe j
--require-copy-watch --require-render-borrow --out <empty directory>`.

## Non-intrusive seek/audio gate

The final `j9` run passed. `player.info()` reported `wav/ima-adpcm`,
`seekable=true`, duration 1,874 ms. The seek promise settled in 20.194 ms
at 1,038 ms (ADPCM block alignment); the status reached 1,224 ms 204.648 ms
after the request, then naturally ended at 1,874 ms. Playback underruns,
source-pool skips, maximum observed starved blocks, and errors were all zero.
The temporary file was removed. The audio-task source made two valid
publications; direct producer-pointer-to-core text copies were 2 × 8 bytes,
with no length mismatch. `core_render_text` and `render_decode_text` copy
counts remained zero. `app_render` p99 was 895 µs in this short diagnostic
run; it is not a comparable 45-second application latency benchmark.

## Display observation and measurement interference

An earlier `j3` run captured the LCD *while playback was active*. Its
`seek_live.rgb565` shows `00:00:01`; the ended capture shows the invalid-source
fallback `--------`. Their 117 differing pixels are entirely within the
96 × 24 source region. That capture streams a full LCD frame over serial
while the WAV producer is on the UI task, and caused 104 audio underruns.
Therefore `j3` proves the clock's visible active/ended states, not audio
performance. `j9` is the independent, capture-free audio/copy gate. A pause
cannot be substituted for active capture: the current non-MP3 pause calls
`player_halt()`, invalidating this output source, so the screen returns to
`--------` even though `status().positionMs` retains the paused position.

The app-area image originally on the Cardputer was verified before testing,
then restored after testing. All three 1 MiB regions at `0x10000`,
`0x110000`, and `0x210000` matched their pre-test digests after restoration.
COM3 was closed. The diagnostic-OFF product build remained 1,995,232 bytes
with 158,780 bytes static DIRAM, unchanged from the prior P4 baseline.

This closes the seekable-format **transport and source-render observation**
part of P3. It does not prove low-heap behavior, another audio track, MP3
seek (still unsupported), or performance while a full serial LCD capture is
in progress.
