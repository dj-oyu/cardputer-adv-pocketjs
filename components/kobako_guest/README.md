# kobako_guest

Caller-driven QuickJS guest for ESP-IDF.

- Public headers: `kobako/guest.h`, `kobako/guest_quickjs.h`
- Targets: ESP32-P4 and ESP32-S3
- Dependency: `espressif/quickjs-ng` 0.14.0
- Ownership: one caller-selected owner task invokes every API except
  `kobako_guest_interrupt`, which is safe to call from a stopping task

The component creates no FreeRTOS task and mounts no kobako capability by
itself. `kobako_guest_frame` calls `globalThis.frame` once and drains the
pending job queue. An explicit `globalThis.frame = null` selects event-driven
mode: each host turn still drains jobs but skips the JS frame call and argument
allocation. A missing frame is still rejected. The QuickJS header is a
version-pinned extension surface;
ordinary firmware includes only `guest.h`.

## Origin and license

This component is derived from `pocketjs_guest`, the ESP-IDF guest of the
third-party [PocketJS](https://github.com/pocket-stack/pocketjs) project
(MIT). This firmware is not part of PocketJS; the component was renamed
from `pocketjs_guest` to `kobako_guest` in 2026-11 together with the rest of
the firmware's `pocket` names. The upstream revision it was copied from is not
recorded here (the design survey pinned `6a0a1b6c91a506c473fc37a0256a47b12eceeca8`).

The MIT terms require the upstream copyright notice and permission text to
travel with the code. They belong in `licenses/PocketJS.txt`, next to the other
third-party notices; that copy is still missing (it was missing before the
rename too).
