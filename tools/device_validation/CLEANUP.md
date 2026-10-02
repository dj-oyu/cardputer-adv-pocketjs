# User-run validation worktree cleanup

`Cleanup-Validation.ps1` previews one explicit run directory by default. It never
finds all folders by wildcard, stops processes, touches a serial port, changes
branches, fetches, resets source, or force-deletes worktrees.

**Two different outcomes:**

1. The helper can unregister and remove clean, detached validation source
   worktrees. It first moves bounded generated build/dependency directories into
   an archive inside that same run. The manifest and run-level logs stay put.
2. Generated files still occupy disk space. Deleting reviewed archived outputs
   is a separate, manual step below. The helper does **not** restore the main
   checkout or remove the separate `pocketjs-validation-tools` scripts worktree.
   Neither of those is an eligible manifest-owned validation worktree.

No cleanup has been run on the user's computer as part of developing this tool.

## Preview the known finished/failed runs

Run from PowerShell in the scripts worktree, with an existing Python 3.9+ and Git
available (Git 2.31+ for absolute common-directory reporting). An active ESP-IDF profile is not needed; an explicit Python executable
can be supplied with `-Python 'C:\...\python.exe'`.

```powershell
Set-Location 'C:\devs\m5stack\pocketjs-validation-tools'
$repo = 'C:\devs\m5stack\cardputer-adv-pocketjs'
$cleanup = '.\tools\device_validation\Cleanup-Validation.ps1'

& $cleanup -Repo $repo -RunRoot 'C:\devs\m5stack\pocketjs-validation\y-001'
& $cleanup -Repo $repo -RunRoot 'C:\devs\m5stack\pocketjs-validation\y-002'
& $cleanup -Repo $repo -RunRoot 'C:\devs\m5stack\pocketjs-validation\integrated-001'
```

Check that these are your actual paths before running. A missing or invalid
manifest is a refusal, not permission to delete the directory. A failed run
that never created worktrees is a no-op: it keeps its manifest and failure logs.

**Keep `integrated-002` for the upcoming device validation; do not clean it now.**
For later cleanup, first finish all work that needs its firmware and worktree.
A `building` or `configuring` manifest always blocks cleanup, even if it is very old. There is
no stale-age override, and an existing integrated-device lock is never removed. An interrupted run left in an active state needs manual
investigation; do not edit its status simply to bypass this guard.

The preview reports every exact worktree path, commit SHA, generated-directory
path, file count, byte count, and `archiveBytes` total. It creates no files.
Treat this as an estimate of retained bytes, **not bytes reclaimed**.

## Remove only after reviewing the preview

Close terminals whose current directory is inside the selected worktree and
verify no build, configure, flash, test, monitor or editor job is using this run.
The helper cannot prove process inactivity or coordinate with older scripts.
Then invoke only the individual reviewed run:

```powershell
& $cleanup -Repo $repo -RunRoot 'C:\devs\m5stack\pocketjs-validation\y-002' -Remove -ConfirmNoActiveProcesses
```

Repeat individually for other reviewed, terminal runs. There is no `-All` or
force option. Both switches are required; neither overrides a refusal.

The helper preflights all entries before moving anything, then rechecks before
removal. It uses only `git -C <repo> worktree remove <exact-worktree>` without
`--force`. Existing main and unrelated worktrees keep their checkout and branch.
Git or OS failure can leave a partially completed cleanup. The error names a
receipt containing the exact moves/removals that succeeded; all archived data
remains in place. Inspect that receipt before retrying.

### Archive policy and refusals

Generated data is moved, not recursively deleted. Only these exact directories
inside each manifest-owned worktree are eligible to move:

- The manifest's exact `build_device_validation` or `build_integrated` directory
- `managed_components`
- `.cache/bmi270`
- `.cache/codecs/minimp3`
- `.cache/codecs/opus-1.6.1`

Every file in those directories survives, including nested Git metadata, build
logs, firmware binaries and any personal files accidentally placed there. The
archive keeps relative paths under its `baseline`, `candidate` or `integrated`
role. Its exact new path is printed as `archiveRoot`, and `receipt.json` records
each source and destination. The original manifest is retained unchanged as
historical evidence; its old artifact/worktree paths are no longer usable after
cleanup. Build and device tooling should use a fresh run next time.

Removal refuses tracked changes (including an IDF-updated `dependencies.lock`),
unknown untracked **or ignored** files outside the directories above, submodules
or nested linked worktrees,
skip-worktree/assume-unchanged flags, attached or locked worktrees, mismatched
commits/repositories, extra registered worktrees under the run, active device
records (including `last-integrated-flash.json`), any `.integrated-device.lock`, and all symlinks/junctions/reparse points found under the run or along
its path. `Repo` must be an exact root and the run must be outside all registered
worktrees. UNC/device paths and wildcard/parent-traversal inputs are unsupported.

If `dependencies.lock` is changed, review and preserve it yourself before
choosing whether to restore that exact file. The helper will not revert it or
silently treat it as disposable. If an unknown file blocks cleanup, move it to a
safe location outside the target worktree after review, then preview again.
Do not follow a refusal with a blanket `git clean`, `reset --hard`,
`worktree remove --force` or recursive deletion of the run root.

## Optional manual disk-space cleanup, after preserving evidence

Worktree removal is not full disk cleanup. Keep the archive until you have
finished comparing the firmware and decided which binaries, configs, dependency
inputs and logs to retain. Keep the run-level logs, original manifest and
`receipt.json` for reproducibility. Also copy any nested build logs or wanted
artifacts out of the particular generated directories you intend to delete.

Use the **actual `archiveRoot` printed by a successful cleanup**, and inspect its
receipt. This example only lists the exact generated directories moved by that
run; it does not delete them:

```powershell
# Replace this placeholder with the exact archiveRoot printed above.
$archive = 'C:\devs\m5stack\pocketjs-validation\y-002\cleanup-archive-<printed-id>'
$receipt = Get-Content -Raw -LiteralPath (Join-Path $archive 'receipt.json') | ConvertFrom-Json
$receipt.mode
$receipt.completed | Where-Object { $_.PSObject.Properties.Name -contains 'archived' } | Select-Object archived, to
Get-ChildItem -LiteralPath $archive -Recurse -File | Where-Object { $_.Extension -eq '.log' } | Select-Object FullName, Length
```

After reviewing and copying out the evidence you want, delete one reviewed
**exact generated directory** at a time. For example, if that exact destination
was in the receipt, and its nested logs/artifacts are already saved elsewhere:

```powershell
# Replace the placeholder with the actual ID; do not delete the whole archive.
Remove-Item -LiteralPath 'C:\devs\m5stack\pocketjs-validation\y-002\cleanup-archive-<printed-id>\baseline\build_device_validation' -Recurse -Confirm
```

Repeat only for the exact remaining generated destinations you have reviewed.
There is deliberately no automatic purge command, wildcard expansion, `-Force`,
or pipeline from receipt data to deletion. Read-only or in-use files should
stop deletion for investigation. Never remove an archive or run directory merely
because its name looks like one of these examples. If you do want to remove the
entire run directory later, first preserve its manifest, logs and receipt
outside that directory and review all remaining contents yourself.

## Offline verification and limits

```powershell
.\tools\device_validation\Test-CleanupOffline.ps1 -Python python
```

This parses the cleanup PowerShell wrappers and runs Python fixtures with mocked
Git. The Python suite can also run independently:

```text
python tools/device_validation/test_cleanup_validation.py
```

Tests cover read-only preview, ownership/layout/status refusals, hidden/untracked
source, junction/reparse detection, retained archives/logs, partial failures,
repeat/no-op runs and the no-force Git argument contract. All data is disposable
test-fixture data, all Git results are mocked, and no firmware/hardware is used.

The initial implementation was tested on Linux with these mocks. **Native
Windows PowerShell parsing/runtime, real Windows Git removal, Windows ACLs and
junction behavior were not executed here.** Run the offline PowerShell test and
review a preview first. Rechecks reduce accidental races but cannot prevent
another process modifying a worktree between checks; stop all related work
before removal. This is deliberately a narrow validation cleanup helper, not a
general repository or disk-cleaning tool.
