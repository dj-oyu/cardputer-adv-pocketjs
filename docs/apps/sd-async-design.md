# SD asynchronous playback experiment

Baseline: `e74d42c`. This worktree holds the shared safety contract before variants diverge.

## Shared lease contract

`pocket_fs_sd_read_lease_open()` runs on the JS owner task. It applies the same
path parsing, grant-before-media check, generation check, directory and size
limits, and writer exclusion as `pocket_fs_read_at()`. It transfers an open
`FILE *` into a caller-allocated `sd_media_read_lease_t` in PERSISTENT mode, or
closes the validation file before returning in REOPEN mode. Both copy the
physical path, size, key and generation. REOPEN opens/seeks/reads/closes once
per refill, while PERSISTENT seeks/reads on its retained handle. The caller
must allocate the lease, compressed ring, and every other producer-visible field together in one
playback session. Once the worker starts, none of those fields may be replaced
or freed until that worker acknowledges exit. The worker may call only
`sd_media_read_lease_read_at()`, then `sd_media_read_lease_ack()` after its last
access. It must not use the mutable global player, picker, JS context,
`sd_media()`, or `pocket_fs_read_at()`.

An owner-task stop first calls `sd_media_read_lease_cancel()`, then waits for
the worker's ACK (`sd_media_read_lease_acked()`). Removal also cancels all
attached leases. The owner then calls `sd_media_read_lease_close()`, which
refuses to close before ACK.
If the wait times out, retain the *whole* playback session, including rings,
path, lease and worker parameters, in quarantine. Poll for its late ACK later;
only then close and free it. A timeout is never permission to free a ring or
unmount the VFS beneath a worker. Pause is not teardown: it retains the
decoder, rings and lease, so resume continues from the same source position.

The lease pins the physical SD mount. `sd_media_unmount()` and
`sd_media_note_error()` immediately revoke the grant and tell active leases to
stop, while `sd_media_service()` performs the actual VFS unmount on the owner
task only after all leases have ACKed and closed. A worker signals a failed SD
command through its lease token; the owner consumes it in
`sd_media_service()`. The owner service runs from `pocket_fs_pump()`, and a new
mount drains any pending unmount first. A timed-out worker therefore also
prevents mounting a new card under its old file descriptor.

The same-file mutation exclusion in `pocket_fs.c` includes attached leases.
Other readers remain allowed, but write, replace, remove and rename of that
path or one of its parent directories answer `BUSY`. This prevents a producer from silently switching to a
different file while a clip plays. SD grant and mount generation are captured
at lease creation and cannot be updated by a worker. A reinserted card needs a
new picker grant and a new lease.

`sd_media_read_lease_read_at()` returns byte count or -1 with an `errno` value.
EOF returns 0. A revoked lease returns `ENODEV`; a card I/O error marks the
lease for owner-task removal and remains a source failure for the player. The
variants must propagate source failures through the existing `P_ERROR` path,
and preserve pause/resume and decode faults. With `KASANE_P0_PROBE=ON`, each
lease counts opens, reads, bytes, maximum read time and reads at or above
200 ms for device comparisons. It emits no per-read serial output. The normal build has
none of those counters or timing calls.

This foundation does not choose a producer schedule or worker priority, and
does not make general `pocket.fs` calls thread-safe. Host tests exercise the
card-free lifecycle model. Physical card removal and teardown timing still
need device verification for each scheduling variant.

## Variant A: REOPEN MP3 reader

`audio.player.play()` creates a session only for an MP3 source on `sd:`. It
owns the two existing three-slot, 2,048-byte rings, their storage, the physical
path lease, the reader task and its terminal state. The priority-4 CPU0 reader
calls `sd_media_read_lease_read_at()` directly into the next compressed ring
slot. Each refill opens, seeks, reads at most 2,048 bytes and closes; there is
no intermediate transport buffer. The decoder stays at priority 6 and audio
task settings are unchanged. Flash MP3 and other codecs use their original
source paths. Header inspection and lease validation at open/play still run on
the UI task; only ongoing SD MP3 refills move to the worker.

Pause retains the decoder state, source offset and both rings. The decoder and
reader park on predicates; notifications only wake the reader to recheck those
predicates. EOF, read error and cancellation are distinct terminal states.
Read errors set compressed EOF so the decoder drains what was published, then
the player reports `P_ERROR`. Cancellation of a blocking read discards its
unpublished slot. Stop cancels the lease and waits 200 ms for worker ACK before
closing the lease or freeing any session-owned memory. A late worker is kept in
quarantine and reaped from the owner task after ACK. If the audio task itself
fails its stop wait, its PCM ring stays quarantined because the sound API has
no late completion ACK to prove it has stopped using that ring. This can retain
the session and pin the mount until reset; it is preferable to freeing live
memory.

With `KASANE_P0_PROBE=ON`, the owner emits one `KSN_P0 SD` line at session
end: actual task core, lease opens/reads/bytes/maximum read time/slow count,
compressed ring low-water slots, stop-to-ACK time, worker stack high-water,
and internal heap free/minimum/largest values. Worker-owned counters are read
only after ACK. Device timing and card removal still require hardware runs;
host ASan/UBSan covers refill/EOF/backpressure/delayed cancellation/error and
the shared lease lifecycle.
