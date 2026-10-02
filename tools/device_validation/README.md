# Windows build and device-validation tools

These are **user-run** tools. `Build-Validation.ps1` still defaults to the pinned
**Y + exact `89a71ae` baseline** comparison; its default action builds two images
and never opens a serial port. The separate
[integrated build-only path](#integrated-feature-branch-build-only) builds one
explicitly selected commit of `codex/vm-improvements`. It does not replace the
pinned comparison or feed the device stages below. There is no
all-candidate matrix, merge, checkout in an existing worktree, clean, forced
operation, firmware reset, automatic restore, or whole-chip erase.

The feature candidates were independently based on
`89a71ae72c721206dfbb63acfeffc74b38dde604`. `origin/vm/main` has since advanced
(observed `bd871f610f65c01bde21f83e8c5dec7888be5227`); that newer revision is
**not** the baseline for these comparisons. `Build-Validation.ps1` does not
rebase or combine candidates; its existing pins are unchanged.

## 1. Get the scripts without changing a busy worktree

Use a separate checkout of the branch containing this directory, or copy this
whole directory somewhere convenient. Scripts create **two new detached
worktrees** under a fresh `RunRoot` using the object database of your existing
`C:\devs\m5stack\cardputer-adv-pocketjs` repository. They never switch its branch.
RunRoot must be outside that repository. Existing directories are refused,
including a partial previous run; retain them and use a new name.

If the pinned commits are absent, fetch the selected branch and baseline first:

```powershell
git -C C:\devs\m5stack\cardputer-adv-pocketjs fetch origin vm/main codex/y-scoped-grid-borrow
```

The script resolves exact SHAs, not the current branch tips. If a server no
longer retains one of these exact commits, stop and obtain that commit rather
than substituting a newer revision. No script fetches automatically.

## 2. Minimal build-only invocation

From this directory, in Windows PowerShell 5.1 or PowerShell 7:

```powershell
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
.\Build-Validation.ps1 -RunRoot C:\devs\m5stack\pocketjs-validation\y-001
```

Requirements: the EIM ESP-IDF **v6.0.1** environment, its own Python, Git,
Xtensa compiler, CMake, Ninja and Node >=16. PlatformIO Python is not substituted.
Inherited C/C++/link flags and SDKCONFIG_DEFAULTS must be unset for a controlled
comparison; a non-S3 IDF_TARGET is rejected.
The existing `tools/prepare_dependencies.py` runs only in the new worktrees and
fetches the repository's dependencies; an initial build therefore needs network
access and disk space for two builds. No dependency directory is shared with a
busy worktree. F's optional host compiler is described below.

Read-only preview, requiring only Git and the local commit objects:

```powershell
.\Build-Validation.ps1 -RunRoot C:\devs\m5stack\pocketjs-validation\y-001 -Plan
```

Every build has its own `build_device_validation/sdkconfig`, with only that
revision's `sdkconfig.defaults`. Existing root sdkconfig, `.cache`, firmware
images, and other worktrees are not reused. A failed command stops the run and
retains partial work/logs; do not delete a busy worktree to retry.

Outputs in RunRoot:

- `manifest.json`: exact source SHAs, flags, build state, paths, image byte sizes,
  image/config hashes; physical/device checks initially `not run`
- tool-version, dependency, build, `idf.py size`, `size-components`, and flash
  budget logs for baseline and candidate; ELF/map and compile commands remain
  in each build directory
- partial manifests/logs on failure. `built` means firmware builds completed,
  **not** that the firmware booted or an optimization won

## Integrated feature branch: build-only

Use `Build-Integrated.ps1` for the combined A/C/E/F/H/Y and compiler changes on
`codex/vm-improvements`. **Supply the reviewed full 40-character commit SHA**;
the branch name is a label, never an automatically resolved input. Obtain that
exact commit first, for example by explicitly fetching the feature branch:

```powershell
git -C C:\devs\m5stack\cardputer-adv-pocketjs fetch origin codex/vm-improvements
. 'C:\Espressif\tools\Microsoft.v6.0.1.PowerShell_profile.ps1'
$commit = 'REPLACE_WITH_REVIEWED_40_CHARACTER_COMMIT_SHA'
.\Build-Integrated.ps1 -Commit $commit -RunRoot C:\devs\m5stack\pocketjs-validation\integrated-001 -Plan
.\Build-Integrated.ps1 -Commit $commit -RunRoot C:\devs\m5stack\pocketjs-validation\integrated-001
```

`-Plan` needs only Git and the local commit object; it prints JSON and creates
nothing. Branch names, short SHAs and missing exact commit objects are refused.
Use `-Repo` if the source repository is elsewhere. As with the comparison,
`RunRoot` must be new and outside the repository. The integrated path also
refuses symlink/junction ancestors; use plain local paths. It creates one
detached `wt-integrated`, with a private `build_integrated/sdkconfig`, and never
switches or cleans the source worktree. Uncommitted changes in the source
worktree are not included: the input is the exact committed tree.

Requirements are the same EIM ESP-IDF **v6.0.1**, EIM Python, Node >=16 and build
tools as above. Inherited `SDKCONFIG` is also rejected. Dependency preparation
runs only in the new worktree and may download the repository's dependencies;
allow space for a full new build. No native host C compiler is needed for F.

This path fixes both optional experiments **OFF**, with no enable switches:

- H: `-DCMAKE_C_FLAGS=-DPOCKET_VM_TYPED_PUT_INT_FAST=0`; the actual QuickJS
  compile command must contain exactly one OFF definition
- F: `-DPOCKET_APP_PRECOMPILE_EXPERIMENT=OFF`; the generated CMake cache must
  confirm OFF and no experimental `.bc` outputs may exist. F's experimental
  tooling is included in source, but runtime bytecode loading is not integrated
- Separate configure and build steps verify ESP32-S3, the H/F settings and
  absence of enabled VM/device diagnostics before firmware compilation and
  again after the build. No OOM/fault-injection workloads are invoked

The new `manifest.json` uses `schema: "pocketjs-integrated-build-v1"` and
`manifestType: "integrated-build-only"`. **`Flash-Validation.ps1` and
`Test-Device.ps1` reject this manifest**, including one marked `built`. Do not
edit its schema to force it through those comparison-only stages. This path
does not open a serial port, flash, reset, or claim device validation.

The run retains tool versions, dependency/configure/build/size/component-size
and flash-budget logs, ELF/map/config/compile commands, and SHA-256 hashes for
the image, ELF, map, sdkconfig, CMake cache and compile commands. The manifest
also records the harness script hashes and, when present, the resolved
`dependencies.lock` hash. IDF may refresh the tracked dependency lock in the
new worktree; that file and worktree status are retained. Any other source
changes stop a successful result. Failures preserve the partial worktree,
logs and failed manifest; retry with a new RunRoot. A successful result means
the integrated image built and passed these configuration/artifact guards,
not that it booted or improved performance.

## 3. Optional app-only flash, one image at a time

These stages accept only `Build-Validation.ps1` comparison outputs.

Do this only when you control the device and have closed all other serial
monitors/tasks. Coordinate with other sessions. These scripts take a shared
`%TEMP%\pocketjs-device-COMx.lock` directory; it protects cooperating invocations
only. An existing lock causes a stop; nothing removes someone else's lock.

Before continuing:

1. Have a **known-good factory app `.bin`** ready as your recovery image. It must
   be an app image, not a complete 8 MiB flash backup. If the installed firmware
   is not reproducible, make your own backup first using your established
   procedure. This harness does not silently read or replace your current image
2. Confirm the installed ESP32-S3 bootloader and partition layout are compatible:
   factory app at `0x10000`, length `0x300000`. Source CSV is checked, but the
   **installed partition table is not read or validated by the script**. It
   refuses full `idf.py flash` because that also replaces bootloader/partitions
3. Identify your actual COM port. There is **no default port**. Enter download
   mode manually using your established device procedure. Flashing necessarily
   erases/replaces the sectors occupied by the app; NVS, fonts, dictionary and
   storage partitions are not addressed by this command

```powershell
$run = 'C:\devs\m5stack\pocketjs-validation\y-001'
$port = 'COM_REPLACE_ME' # Replace with the port you verified; placeholder is rejected
$recovery = 'C:\path\to\your-known-good-app.bin'
.\Flash-Validation.ps1 -RunRoot $run -Role baseline -Port $port -RecoveryImage $recovery -AllowFlash -ConfirmCompatibleBootloaderAndPartitions
```

The script uses the repository's esptool write/verify flow at `0x10000`, with
`--before no-reset --after no-reset --no-stub`. Image chip information and image
hashes are checked before opening the port. No device is reset automatically;
if connection fails it stops rather than trying a reset/erase fallback.
The recovery image is copied and hashed into that flash run's log directory.
Nothing automatically flashes it afterward. **Manually reboot to normal mode**
and wait at the home screen before the next stage.

## 4. Optional ordinary smoke and GRID LAB measurements

```powershell
.\Test-Device.ps1 -RunRoot $run -Role baseline -Port $port -AllowDevice -ConfirmRunningImage -Grid
```

`-ConfirmRunningImage` is your acknowledgement that you rebooted the image just
verified by the flash stage. The harness validates that stage's role/port/hash;
it does **not** pretend to read a runtime ELF identity from ordinary HELLO logs.
DTR/RTS are disabled before opening serial. OS/driver behavior still warrants
watching the board. Start with an idle home screen and no unsaved work.

Automated checks:

- Three ordinary HELLO launch/count/stop cycles, the `local.hello` identity,
  frame/home markers, bounded waits, and observed post-stop memory
- Optional `-Grid`: that revision's existing ordinary GRID LAB pipeline runner,
  which validates four complete 180-frame window/full AUTO/SCALAR arms, backend
  labels, timing consistency, and paint windows. Additional log checks require
  mode 3/4/5 scalar/PIE measurement and GATHER/AFFINE equality
- Raw serial logs and JSON results are retained even when the HELLO check fails;
  the existing GRID runner also saves partial capture in `finally`

This deliberately does not run `smoke_device.py` (its later cases are fault
recovery), `stress_app.py`, diagnostic builds/keys, allocation-failure tests,
OOM tests, or the full contract matrix. HELLO alone is a smoke test, not a VM
conformance test. Memory values are observations, not a leak verdict with an
arbitrary threshold. Audio/menu keys may make normal sounds; persistent settings
are not changed. Successful runs return home; on failure use the physical Back
key if needed. Nothing resets or restores the board on failure.

After baseline, repeat **flash -> manual reboot -> test** with `-Role candidate`.
Only the last verified role can be tested. Each invocation gets a new log folder,
so repeated A/B/A samples do not overwrite earlier results. Reflash baseline
explicitly for the final A sample; this never happens automatically.

## 5. Read the measurements honestly

Keep board, power, settings, Wi-Fi/autosync state, temperature and app workloads
consistent. Let boot activity settle, then run repeated A/B/A samples; use at
least three runs per image before interpreting a trend. Compare same-arm
`grid-summary.log` values, raw registrations, and `GRID_APP PIPE`/`MEM` lines.
No script outputs a speedup or pass threshold for performance.

`source_ms`, `run_ms`, `native_total_ms`, paint stages, heap free/largest and
registration fields answer different questions. `copyUs` includes capture/bind
preparation and DEST seed, not just memcpy; Y changes capture ownership, and
`totalUs` excludes input cleanup/return-value construction. Timing labels alone
do not prove a native copy went away. Cross-image cache layout effects can
change times. A larger free heap is not by itself proof of leak freedom.

Y changes `grid.measure` from `measure(handle, repeats, strategy)` to
`measure(handle, buffers, params, repeats, strategy)`. Its own `grid_lab.js`
is updated and is built unchanged; no old JS payload is injected into the new
image. Measurements use scratch destinations, so equality is not a test that a
previous rendered DEST-dependent fold is reproduced. The ordinary workload does
not cover every scoped-input contract. See the Y branch's
`docs/perf/kasane-grid-scoped-borrow-host.md` for scope and remaining limitations.

Manual checklist, for **each image**, record alongside its result:

- Physical LCD looks correct during HELLO and each GRID LAB mode, with no tearing,
  corruption or unexpected artifacts. Pre-SPI software logs do not read the LCD
- Physical navigation, Enter and Back work; expected sound and app return work
- Normal DERBY WATCH/other user-relevant apps launch, draw and return; record the
  app and any issue rather than interpreting the small smoke test as full coverage
- Note anything unexpected, board/setup conditions and the exact image/role
- Restore your desired normal app explicitly when finished. Use your existing
  reviewed recovery procedure with the retained known-good image; the script
  does not assume the candidate or old baseline is your desired daily firmware

## Other candidates (one per fresh RunRoot)

| Candidate | Branch / exact commit | Meaning of this harness |
|---|---|---|
| A | `vm/cloud-v2-a-frame-entry` / `22e2e14f2ce4da33c4b1dabdc4375b16f81c6406` | Build, ordinary smoke/grid observations; no proof of full frame-entry semantics |
| C | `vm/cloud-v2-c-parser-shrink` / `ea72fbb90c8551f2e069c550b0f14a08bde31150` | Build, normal startup observations; parser peak savings are not instrumented here |
| E | `vm/cloud-v2-e-array-trim` / `c17d3c5cfee9996994f50bc4bdbc4b15e4058e75` | Build, ordinary app observations; no exhaustive array behavior test |
| F | `vm/cloud-v2-f-precompile` / `1e14ab786f7db671fd12cc7ffa779b3f8350d718` | Build-only, experiment OFF unless explicitly enabled; runtime source loader remains unchanged even ON |
| H | `vm/cloud-v2-h-typed-put` / `85798a73ccf75ef243dde3728fe163f58420e27a` | OFF unless `-EnableTypedPut`; manifest and actual QuickJS compile command distinguish it |
| Y | `codex/y-scoped-grid-borrow` / `c5a4993f399e57c1df31054373f1c42cf62b9e57` | Default; ordinary HELLO + optional matched GRID LAB measurements |

Examples:

```powershell
.\Build-Validation.ps1 -RunRoot C:\devs\m5stack\pocketjs-validation\h-on-001 -Candidate H -EnableTypedPut
.\Build-Validation.ps1 -RunRoot C:\devs\m5stack\pocketjs-validation\f-off-001 -Candidate F
# Optional F requires an already installed native Windows gcc/clang, not Xtensa.
# Ensure that compiler's runtime DLL directory is already in this shell's PATH.
.\Build-Validation.ps1 -RunRoot C:\devs\m5stack\pocketjs-validation\f-on-001 -Candidate F -EnablePrecompileExperiment -HostCc C:\msys64\ucrt64\bin\gcc.exe
```

H uses `-DCMAKE_C_FLAGS=-DPOCKET_VM_TYPED_PUT_INT_FAST=1` in a fresh candidate
build and verifies that define in `compile_commands.json`; OFF explicitly uses
`=0`. The baseline has neither define. To compare H OFF versus ON too, keep
separate runs. F uses `POCKET_APP_PRECOMPILE_EXPERIMENT` and
`POCKET_APP_HOST_CC`; it checks for generated `.bc` files. The F target loader
is **not integrated** and the flash/test stage refuses F's candidate image.

## Offline checks and validation limits

```powershell
.\Test-Offline.ps1 -Repo C:\devs\m5stack\cardputer-adv-pocketjs
# Python-only mocks (also works on Linux, no pyserial required):
python .\test_safe_smoke.py
# Integrated path's Python-only static guardrails:
python .\test_integrated_build.py
```

`Test-Offline.ps1` parses every PowerShell file with PowerShell's own parser,
runs the serial mocks and integrated-build static guards, checks `Invoke-Logged`
with real Python processes that exit 0/7/0 (including stdout/stderr logs and a
caller-local exit-code shadow),
verifies both device stages reject an integrated build-only manifest before
toolchain/port access, and optionally exercises the read-only Y and integrated
plans plus full-SHA input rejection when `-Repo` is supplied. The integrated
plan test uses that repository's HEAD only as an available commit object; it
does not certify its feature contents. It does not run IDF or access
any port. The Python tests exercise normal cycles, split
serial markers, wrong app, ANSI colors, error logging, bounded timeout, diagnostic-key
rejection, missing/equal/unequal GRID records, CLI opt-in and static guardrails.

At authoring time the cloud environment ran Python mocks and syntax/whitespace
checks only. PowerShell, Windows EIM/ESP-IDF, esptool and a device were unavailable:
**PowerShell runtime, firmware builds, flashing and hardware behavior are not
claimed tested**. Run the offline PowerShell check first on your Windows machine.
