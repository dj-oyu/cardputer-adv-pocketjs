# D5 KSV1 file stream prototype

`kasane.video.open(width,height)` still creates the three-slot, 4096-pixel
RGB565 image. After a person grants an SD folder through the picker, an app may
call `kasane.video.streamStart("sd:/.../clip.ksv")`. The chosen path is checked
by PocketFS on the JS owner task. Start opens one persistent SD read lease; it
does not grant a folder. The picker must be closed before start. With the MP3
reader's transient reopen handle, the two readers use at most the two existing
SD file handles. A grant or card removal revokes the lease.

The container is deliberately small and versioned. All integers are little
endian. The 16-byte header contains `KSV1` (4 bytes), width and height (u16),
RGB565 frame byte count (u32), and zero reserved field (u32). Each record is
PTS in microseconds (u64), payload byte count (u32), then complete packed
RGB565 pixels. PTS must increase strictly. Dimensions must match the already
open video and contain 1–4096 pixels. Every byte count must equal
`width*height*2`; truncated records, bad order, and trailing partial headers
are errors. There is no compression, padding, index, or audio track.

`streamStart()` returns after lease creation; the worker validates the header
asynchronously. `streamState()` reports `reading`, `end`, or `error` after
that work, and `closed` after stop. The app calls `streamPoll(clock_us)` on an
owner turn to hand over at most one completed frame and select the latest frame
at or before the supplied clock. A true return means Kasane has a selected
candidate, whose lifetime still ends only at display ACK. `streamPause(bool)`
pauses input reads; `streamStop()` cancels the worker and returns true if its
lease was closed within 200 ms. A false stop result means the session was
quarantined until its worker acknowledges cancellation. The ordinary frame
path does not perform the stop wait. Owner turns reap acknowledged quarantines
after `pocket_fs_pump()`. An app should call `streamStop()` after `end` or
`error` to release the persistent file handle.

The worker runs on core 0 at priority 4. It owns one frame-sized staging
buffer and never touches QuickJS, Kasane, or the three-slot pool. Each worker
step scans at most eight record headers. It reads one payload only after the
scan reaches the first future record or EOF; every older payload is skipped.
It may replace a staged frame that the owner has not borrowed;
pool BUSY also drops that input frame. The owner copies at most one complete
frame to an unpinned slot per poll. A borrowed staging frame cannot be
overwritten until the owner releases it. Kasane's pending candidate and old
committed image remain pinned through failed LCD transfer and repair. Starting
a new stream resets the PTS epoch only when no candidate or repair is pending.
The app must reset the timeline before a backward seek; this prototype has no
seek operation.

Host checks cover dimensions, truncated records, strict PTS, future-header
caching, an overdue burst with only the final payload read, and
candidate discard/repair. The QuickJS adapter test covers pause, poll, stop,
and the existing display ACK path with a mocked SD worker. The standalone
SD diagnostic completed two consecutive real-device runs after pacing its
test-file generation: each created and removed a 31,456-byte KSV1, selected
54 frames with PTS 0–1,966,647 µs, reached EOF, and stopped with ACK in
8–9 µs. Worker stack free was 2,020/1,980 bytes. The first version generated
all sample bytes in one JS drain and hit the app's `RUNAWAY` limit; each
524-byte record is now made in a separate timer turn. Evidence is
`.cache/d5-sd-stream-yield-device-20260928/serial.log`.

The host checks do not prove the FreeRTOS partial-read race, a delayed ACK
beyond 200 ms, external revoke while paused, LCD failure during a pending
stream frame, or physical audio/video phase on hardware. A separate D6 device
gate ran simultaneous SD MP3 and SD KSV1 for ten seconds, using the audio
position as the video selection clock; see the dynamic rendering roadmap.
The parser host test injects a short payload read and proves it can modify
the staging bytes without returning a frame. The worker now clears its staging
slot and records terminal error before a borrow can proceed, but this race
still needs a FreeRTOS fault-injection run on hardware.

For a device run, build with `-DKASANE_D5_SD_STREAM_PROBE=ON`. This replaces
the existing VIDEO LAB embedded source for that diagnostic image; it does not
add a menu row or change the normal image. The probe creates only
`sd:/__ksn_d5_stream_probe_20260928.ksv` within the folder granted in its
picker. It refuses a name collision, commits only after all writes, and calls
`streamStop()` before attempting removal. A Back or forced stop before cleanup
can leave its committed file behind. Run the explicit
[`video_stream_recover.js`](../../apps/kasane/video_stream_recover.js) from
the Code screen after granting the same folder; it compares the full file
against the deterministic probe bytes and removes it only on an exact match.
The normal probe refuses collisions until recovery succeeds.

The two-run device gate is
[`run_d5_sd_stream_device.ps1`](../../tools/kasane_contract/run_d5_sd_stream_device.ps1),
which drives the existing VIDEO LAB row and requires increasing selected PTS,
EOF, worker stop ACK, file removal, and a clean second creation. It writes a
serial log and restores/verifies the normal app image with `HOME_READY` in its
`finally` path. It writes only the app partition and makes no flash backup.
Wait for `D5_STREAM REMOVED` and `D5_STREAM DONE reason=end` before pressing
Back. A failed run may leave the probe file; use the exact-content recovery
script above before retrying.

The first COM3 attempt (2026-09-28) reached `PICK 2 folders` and `GRANTED
music`, then the app stopped with `RUNAWAY one drain spent 256340 us over 3
jobs in 31 turns` before logging `CREATED`. It did not verify a stream frame.
The probe now generates one 524-byte record per write and yields through a
20 ms timer after each write. Recovery compares one read chunk per timer turn
without constructing the full 31 KiB sample. The rebuilt diagnostic image
SHA-256 is `6FFBA162B99D957A8010BF1863D9B4BDA7C7AAC5BDED2566B5B7FBDDEE1A6BEE`;
The paced version then passed twice on hardware: 54 selected frames per run,
PTS 0–1,966,647 µs, EOF, worker ACK, exact file removal, and clean recreation.
The serial log is `.cache/d5-sd-stream-yield-device-20260928/serial.log`.
