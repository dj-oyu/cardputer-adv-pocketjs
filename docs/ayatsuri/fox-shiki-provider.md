# Experimental native fox / Shiki presentation integration

This feature is default-off. The public changes contain no model arrays or asset
binary. Enabling it requires a separately supplied local private renderer/model
overlay; an ordinary public checkout is not a complete private fox distribution.
No fallback to publishing private inputs is permitted.

## Call path

`local.fox` native app -> app-session-installed `privateModelBackend.commit` ->
`shiki_owner` -> `pocket_proc_fox_publish` -> Kasane presentation -> Shiki image
`read_rows` / `read_span` or backdrop -> `shiki_provider` -> fixed fox compositor.
The native app is only registered in the explicit `FOX_SHIKI_PROVIDER` build.
The old board_fox presenter, SPI mock and QEMU diagnostic owner are not linked by
this profile. Kasane remains the only display presenter.

The provider uses direct C calls, not a new function-pointer dispatch layer.
The existing Shiki image-port interface is retained. There is no prerasterized
full-screen image: the compositor writes into the presenter's requested strip.
Shiki's existing span-band cache is used only when a span read is requested.
The PR #133 ordered bitmap selector is shared by Shiki and fixed triangle band
selection; this does not by itself establish a speed improvement.

## Ownership and failure

The owner allocates one scratch workspace and two immutable snapshot slots on
first use. A published snapshot remains pinned through presentation. Failed
candidate preparation or publication retains the old committed snapshot; the
single scratch is regenerated for the selected snapshot when necessary. IO
failure retains the pending snapshot for retry. No-work publication is logically
acknowledged as in Shiki's normal commit, not proof of physical LCD delivery.

An eight-row request is split into at most two four-row compositor calls. On
failure, destination memory may be partially written. The presenter must not
send that failed request. The actual host Kasane renderer test injects failure
at row four and verifies zero sends for the failed first eight-row request.
Already transmitted earlier strips are not rolled back by this guarantee.

STOP closes new admissions before guest stop hooks. Normal Kasane reset joins
and releases provider pins. Owner storage is freed only after the lease pool is
drained; a busy/failed detach retains storage and blocks a new owner session.

## Build and remaining gates

Local profile: `-DFOX_SHIKI_PROVIDER=ON -DFOX_LOCAL_Q29=ON`. Asset verification is
an always-run build dependency. Public prepared/Q7 experiments currently have
conflicting symbol names and are rejected in this profile rather than silently
combined. Separate build/sdkconfig directories are required as usual.

Host evidence includes the actual Shiki row/span/backdrop entries, fixed-renderer
image comparisons, real Kasane core/renderer with a spy display port, and owner
failure/retry/drain tests. Host publication and display transport boundaries are
substituted in those tests. Full ESP-IDF build, app-session/RTOS execution, target
memory/stack, DMA lifetime, and fox FPS remain unverified. Shiki's pre-existing
real-device measurements must not be presented as fox measurements.
