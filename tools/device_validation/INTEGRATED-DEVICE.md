# Integrated image: explicit user-run device checks

`Flash-Integrated.ps1` and `Test-Integrated.ps1` are the separate device path for
a completed `Build-Integrated.ps1` run. They can use an already-built v1 run,
including the image built from `22f6d54a5c2fad6f7f40a8d5aa7cea7d7f371c51`.
**No firmware rebuild is needed merely to obtain these scripts.** Keep the new
tool directory outside the retained build worktree and copy the whole directory,
including its Python helpers. Do not modify that worktree to install the scripts.

This does not change the old comparison path: `Flash-Validation.ps1` and
`Test-Device.ps1` still reject integrated manifests. There is no `-Role` choice
here: every record says `integrated`, with its full source SHA and binary SHA256.
The build manifest remains `integrated-build-only`; a separate successful flash
record plus explicit user attestations are required before device tests.

## 1. Keep control of the board

Only you run these commands on your device. Close IDF monitor, editors' serial
monitors, other test runners and any agent using the port. Confirm the board is
free and take control of it yourself. The scripts cannot discover every other
serial user. Do not run two harnesses against one board under different port names.

Retain an **ESP32-S3 factory app binary**, not a whole-flash backup. By default,
supplying it means you attest it is known-good. If its working state is unknown,
add `-AllowUnverifiedRecovery` to retain it explicitly as an **unverified recovery
candidate** instead. Its chip, size and hash are still checked, but those checks
do not prove it boots: **there is no verified rollback guarantee**. A full-device
backup and proof that this candidate works are not required by that option.
You must already have a compatible bootloader and the matching partition layout:
factory app at `0x10000`, size `0x300000`. The scripts do not install or repair
bootloaders, partition tables, NVS, dictionary, fonts or storage. `write-flash`
erases only sectors occupied by the app image at that address. An app-only flash
is still a destructive replacement of the current app firmware.

Before using hardware, run the offline checks in PowerShell 5.1 or 7:

```powershell
.\Test-Offline.ps1
# Or choose an existing Python executable explicitly:
.\Test-Offline.ps1 -Python 'C:\path\to\python.exe'
```

The new PowerShell fixture substitutes every external-command boundary and uses
temporary files. It does not invoke IDF, esptool, pyserial or any real serial port.
Its Python tests use mock serial transcripts and synthetic build records.

## 2. Check the exact completed build

Keep the original RunRoot, its `manifest.json`, `integrated-build-flags.log`,
`wt-integrated`, and build outputs in place. Do not move them: the manifest has
absolute artifact paths. Select the full SHA that you actually built, not the
newer branch tip containing the device tooling.

```powershell
$run = 'C:\path\to\retained-integrated-run'
$commit = '22f6d54a5c2fad6f7f40a8d5aa7cea7d7f371c51'
python .\verify_integrated_run.py --run-root $run --commit $commit
```

This command is read-only and never imports serial. It requires:

- The integrated v1 schema, successful manifest and exactly one completed build,
  with `configurationVerified` exactly true and the requested full commit
- H, F and runtime precompile explicitly false, and the controlled CMake options
- All six recorded artifacts, with matching paths, nonzero sizes and SHA256:
  app `.bin`, `.elf`, `.map`, `sdkconfig`, `CMakeCache.txt`, `compile_commands.json`
- The recorded dependency-lock hash, unchanged pinned source HEAD, and no source
  changes other than the already-recorded dependency lock
- The expected factory partition, current embedded HELLO/GRID menu positions and
  scoped-input GRID API. A future firmware change to this contract fails closed
- Ordinary ESP32-S3 config, F OFF, no enabled diagnostic probe option or generated
  F bytecode, and QuickJS H=0 including referenced compiler response files
- Exact post-build QuickJS flag/response hashes from `integrated-build-flags.log`.
  Both PowerShell 5.1 UTF-16 and PowerShell 7 UTF-8 logs are supported

Original recorded build-tool hashes need not equal the newer device-tool hashes.
Nothing rewrites the manifest, reconfigures the build or runs a recorded compiler
command. These are local artifact-integrity checks, not signed provenance.

## 3. Explicit app-only flash, then manual reboot

Activate your existing EIM ESP-IDF **v6.0.1** PowerShell profile. The script uses
that environment's Python and checks the IDF version; it never guesses a fallback.

Choose the correct free port yourself, put the board into download mode manually,
and then run:

```powershell
$port = Read-Host 'Which free device port do you control? (for example COM7)'
$recovery = 'C:\path\to\known-good-factory-app.bin'
.\Flash-Integrated.ps1 -RunRoot $run -Commit $commit -Port $port `
  -RecoveryImage $recovery -AllowFlash `
  -ConfirmCompatibleBootloaderAndPartitions -ConfirmDeviceFree -ConfirmDownloadMode
```

If `$recovery` is only a candidate whose working state is unknown, append
`-AllowUnverifiedRecovery` to that command. The script records
`recoveryConfidence=unverified-candidate`, emits a warning, and retains the copy
as `unverified-recovery-app.bin`. Without that switch it records
`user-attested-known-good` and retains `known-good-app.bin`; neither label is an
independent device test of the recovery file.

The script verifies the artifacts, keeps private snapshots of both app images,
checks their ESP32-S3 image headers, and writes/verifies **only `0x10000`** with
esptool `--before no-reset --after no-reset --no-stub`. A failed verify invalidates
the previous integrated flash success. No command resets, erase-alls, restores or
reboots the board, including on failure. The recovery copy is retained for your
own explicit recovery procedure; it is never flashed automatically.

When flash verification succeeds, the port lock is released. **The device is
yours: manually reboot, wait for home and let boot/Wi-Fi activity settle.**
Do not change the flashed image or let another tool flash it between these steps.
If you do, repeat the integrated flash and manual reboot before testing.

## 4. Ordinary HELLO, optional embedded GRID LAB

```powershell
.\Test-Integrated.ps1 -RunRoot $run -Commit $commit -Port $port `
  -AllowDevice -ConfirmDeviceFree -ConfirmRunningImage -Cycles 3
# To include one complete ordinary GRID LAB cycle:
.\Test-Integrated.ps1 -RunRoot $run -Commit $commit -Port $port `
  -AllowDevice -ConfirmDeviceFree -ConfirmRunningImage -Cycles 3 -Grid
```

The test rechecks build evidence and requires a matching successful
`last-integrated-flash.json` for this run, source SHA, manifest hash, binary hash
and port. `-ConfirmRunningImage` is **your attestation** after the manual reboot;
serial app markers do not cryptographically identify the running firmware.

The Python runner selects embedded HELLO, checks its identity, frame, Enter count,
stop and return-home markers for each cycle. Optional GRID selects the existing
embedded GRID LAB and waits for its normal automatic sequence: equality for
MEASURE/ROUTES modes 3–5, followed by four complete 180-frame WINDOW/FULL and
AUTO/SCALAR pipeline arms. It uses the firmware's current scoped-input API;
there is no uploaded JavaScript payload. Full serial logs, complete post-stop
memory lines and summaries are retained, including partial logs on failure.

Only normal menu/Enter/Back keys are used. There are no diagnostic, OOM,
allocation-failure or stress cases, and no hidden setting/storage changes.
Normal menu sounds and normal firmware background activity may occur. Successful
runs return home. On failure the script stops; use physical Back if needed and
decide on recovery yourself. Nothing keeps sending keys to recover automatically.

## Logs, locks and interpretation

Each invocation creates a fresh `flash-integrated-*` or `device-integrated-*`
directory. Preserve `result.json`, `preflight.json`, the raw serial transcript,
image hashes and flash ID when reporting a result. Never edit a record to force a
pass. A failed early preflight can leave only its partial log directory.

An atomic `.integrated-device.lock` under RunRoot serializes that run's flash/test
records. The existing shared `%TEMP%\pocketjs-device-<PORT>.lock` serializes the
port against this harness and the comparison harness. Neither lock is stolen or
expired automatically. A forcibly terminated process can leave one behind:
inspect who owns the device and whether any process is active before manually
removing a stale lock. Do not delete or clean an active run.

Markers passing is an ordinary smoke result. Physical LCD, keys and audio still
need your inspection; software captures cannot read the physical LCD. Record
your normal DERBY WATCH/other app checks separately. Memory/timings are observations,
with no leak threshold or performance-speedup claim. H remains OFF, F's runtime
loader is not integrated, and these checks are not full VM or GRID conformance.

Restore your desired daily firmware only through your own explicit reviewed
procedure. The scripts leave the selected integrated image on the board.

## Validation limits

Cloud validation covers Python fixtures, static entry-point contracts and
whitespace/syntax checks. PowerShell native parsing/runtime, Windows EIM/ESP-IDF,
esptool operations and hardware behavior must be checked on Windows and the
device; they are not claimed tested in the cloud. Passing an offline fixture
does not establish that a physical flash, reboot or app run succeeded.
