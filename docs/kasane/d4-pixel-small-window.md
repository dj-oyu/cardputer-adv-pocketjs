# D4 small-window pixel image API

`kasane.pixel` exposes one changing image resource per APP session. Its first
step intentionally accepts a narrow workload: width 1–112, height 1–63,
one image resource, up to eight straight-line instructions, and no underlay.
The regular Kasane image node can place this resource at any clipped position.
`open` fixes its dimensions until APP reset.

```js
const image = kasane.pixel.open(8, 8);
const code = new Uint16Array([
  1, 0, 0, 0, 0xf800, // IMM r0 = red RGB565
  1, 1, 0, 0, 255,    // IMM r1 = opaque alpha
]);
const params = new Uint16Array(8);
const accepted = kasane.pixel.stage(code, params, 0, 1);
```

`stage(code, params, colorReg, alphaReg)` takes exactly five `Uint16Array`
words per instruction: opcode, destination register, operand A, operand B,
immediate. It copies the complete program and eight parameters before
returning. Opcodes match `ksn_pixel_opcode`: 1 `IMM`, 2 `X`, 3 `Y`, 4 `PARAM`,
6 `ADD`, 7 `SUB`, 8 `MUL`, 9 `SHR`, 10 `AND`, 11 `OR`, 12 `XOR`. Opcode 5
`UNDERLAY` is explicitly unsupported. Arithmetic wraps at 16 bits; alpha
values above 255 clamp to 255. The native validator rejects unread registers,
invalid parameters, shifts beyond 15, invalid opcodes, and missing outputs.

`stage` returns `false` while Kasane has an unfinished submission or the
previous pixel candidate is pending. Invalid arguments throw. The accepted
program becomes the image's candidate for the next presentation. An I/O
failure retains that candidate for the same-pixel retry. A successful
presentation promotes it to committed. Every node referencing this single
resource reads through the same logical frame, so all such nodes move to the
new slot together. APP reset detaches the resource before releasing the pool.

The host QuickJS/renderer contract is
`python tools/kasane_contract/run_pocket_pixel_qjs.py`. It exercises JS
registration and validation, two retained nodes, a second-band I/O failure,
same-candidate retry, and the next generation. The pre-existing span
equivalence and device diagnostic measurements are recorded in
`dynamic-rendering-roadmap.md`. The ordinary JS-to-Kasane path also passed on
COM3: two nodes displayed the maximum 112×63×8 program, the injected second
LCD send failed once and then succeeded on retry, and the APP stopped and
restarted cleanly. Successful candidate presentations in the first run took
16.3–20.5 ms; the UI task's minimum free stack was 23,708 bytes. The serial
evidence is `.cache/d4-pixel-app-device-20260928a/serial.log`. Full-screen
programs, independent frame leases, underlay, depth, and arbitrary JS callbacks
remain outside this contract.

The device gate uses `-DKASANE_D4_PIXEL_APP_PROBE=ON` with the normal VIDEO LAB
menu slot replaced by `pixel_lab_probe.js`. The APP stages 112×63×8 through
the public JS API and draws one resource in two nodes. The diagnostic display
adapter fails its second send once, logs the retained candidate and successful
retry, and records the UI task's stack high-water mark. Run
`python tools/kasane_contract/run_d4_pixel_app_device.py --port <port> --out <log>`
after flashing the diagnostic image; it requires two complete APP launches
and rejects a missing failure, retry, progress marker, or low stack margin.
The probe is disabled in the ordinary image.
