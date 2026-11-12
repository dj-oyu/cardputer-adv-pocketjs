# Explicit synthetic Scene skin sample (opt-in)

Six strip vertices / two joints plus cold background mesh; public synthetic AYNM
from the existing packer. No character/private assets, new wire format or importer.

`skin_entry.js` loads production Motion, Scene and this sample via the named
optional chunks. `startSkinSample(coldBuffer)` returns `{frame(timeMs),stop()}`.
The caller owns the finite millisecond clock; repeated time pauses, decreasing time
seeks. No timer or implicit Motion advancement. Stop is idempotent; later frames
throw. A submitted renderer snapshot may outlive stop. Native scene/model ownership
is released on startup failure. The app does not reset the shared renderer.

Generate a standalone bootstrap and chunk table:

```
python3 apps/skinsample/prepare.py .cache/skin-optional
```

The generated `skin_bootstrap.js` installs the image resource and starts the sample
from the cold synthetic bytes. Invoke `skinSampleFrame(timeMs)` from the diagnostic
clock and `skinSampleStop()` on diagnostic shutdown. It deliberately does not install
an unverified device frame hook. Register the diagnostic session as `local.skinsample`
and explicitly append `apps/skinsample/optional_chunks.txt` through
`POCKET_APP_CHUNK_LISTS` when integrating a diagnostic firmware. The file is not
named `chunks.txt`, so ordinary builds, HOME and defaults remain unchanged.
Registry selection and QEMU stdio are separate firmware-lane acceptance, not claimed
by the host test. Do not silently reuse another app's registry identity.

```
python3 tools/ayatsuri/skin/run_optional_app.py --no-leak-check
```

Host acceptance executes the shipped entry and libraries on real QuickJS/native
bindings/renderer with a named chunk dispatcher shim. It does **not** test the
production C chunk loader. Three start/stop cycles each visit 120 times forward,
120 backward and 24 paused, with panel hashes equal across seek/pause/restart,
stable released-cycle memory and warmed frame allocation counts, retry after IO failure, pending snapshot on stop,
and zero final requested allocation ledger. This is deterministic lifecycle evidence,
not an independent skin/math oracle or device RAM/FPS result. ASan/UBSan remain on;
the flag disables only LSan when ptrace prevents it.
