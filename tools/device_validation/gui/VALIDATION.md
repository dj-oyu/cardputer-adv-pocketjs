# GUI validation record

Cloud checks on 2026-10-02 (synthetic files and mocked commands only):

- `uv lock --check`: passed; exact direct versions and transitive hashes locked
- `uv sync --frozen`: passed in this GUI's isolated `.venv`
- `uv run --frozen python -m unittest -q test_gui`: 27 tests passed
- Existing `device_validation` Python suites: 130 tests passed
- `node test_ui.cjs`: passed (minimal DOM and in-memory API mocks)
- `node --check static/app.js`: passed
- Python compile and `git diff --check`: passed
- `launch.py --help`: required project and optional IDF path documented correctly
- Synthetic loopback FastAPI server startup: passed; subsequently stopped
- Independent safety review: reported stylesheet routing, process ownership on
  logging errors, and late COM identity checks. All three were corrected and
  covered by regression tests; rereview found no remaining release blocker

The tests cover missing/changed evidence, changed/disconnected COM identities,
recovery/image selection, plan expiry, wrong digest and missing acknowledgements,
one-use execution/replay, concurrent device reservations, command failure,
logging errors, bounded discovery/logging, symlink exclusion, exact downstream
pins, authentication/Host/Origin/CSRF boundaries, body limits and static assets.
UI tests cover explicit selection, action-specific review, stale/expired plans,
unknown acknowledgements, running-job locks and API failures.

## Still unverified

- Actual browser rendering, layout and accessibility: Chromium's local IPC was
  unavailable and the cloud browser refused loopback navigation with
  `ERR_BLOCKED_BY_CLIENT`. No screenshot or browser-render pass is claimed
- Windows 11, EIM environment resolution on Windows, PowerShell 5.1/7 runtime and
  native process/encoding behavior. `Test-IntegratedOffline.ps1` includes new
  hash/flash-ID/COM mismatch fixtures, but PowerShell is unavailable in the cloud
- Actual ESP-IDF/esptool image-info, USB enumeration on Windows, flash/verify,
  reboot, HELLO/GRID, physical LCD/keys/audio

Run `..\Test-Offline.ps1` on Windows and inspect the GUI before approving any
hardware action. There was no user-PC access, physical device operation, firmware
rebuild, diagnostic/OOM/fault injection, reset, restore, push, or publication in
these checks. Passing mocked tests does not establish a successful device run.


## Discovery/direct-import regression checks

The follow-on discovery fix passed 33 GUI Python tests and the updated offline
DOM/API harness, plus the existing 130 Python tests. New synthetic cases cover
`.cache/flash_backup` in both project and registered worktrees, other cache
exclusion, linked backup directories, app-only backup names, malformed merged /
bootloader images, explicit valid/missing/symlink paths, imported-candidate
retention/revalidation, changed recovery hashes and authenticated import routes.
No real user path or firmware is included in these fixtures. Hardware and browser
rendering were not exercised by this fix.
