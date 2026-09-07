#pragma once
#include "esp_err.h"
#include "quickjs.h"

// pocket.ble — section 12 of docs/common-api.md: BLE Central.
//
// The surface is NOT implemented on this board, and this file exists so that an
// app can find that out the way section 2 asks: a name it can feature-test and
// a method that rejects with UNSUPPORTED, rather than a TypeError about reading
// a property of undefined.
//
// The reason is measured, not assumed. Section 12 says "ホストスタックは
// NimBLEを第一候補とし、採用時のRAMを測定する", and section 18 records that the
// measurement had not been made. It has been now. Three builds of this tree at
// one commit, differing only in configuration:
//
//   A  as shipped                                   DIRAM 172,487 of 341,760
//   B  A + `bt` in the component set, BLE OFF       DIRAM 191,319
//   C  B + NimBLE central ON (sdkconfig.ble)        DIRAM 208,251
//
// C - B is NimBLE: +16,932 bytes of static DIRAM and +105,408 of flash code.
// B - A is not BLE at all -- pulling `bt` into the build needs the COMPONENTS
// variable, which turns MINIMAL_BUILD off, and that alone costs 18,832 bytes.
// (Worth knowing on its own: MINIMAL_BUILD ON is paying for itself six times
// over what the rest of this file is about.) A production BLE build would keep
// MINIMAL_BUILD and reach roughly 189,419 bytes of static DIRAM, against the
// 194,560 ceiling tools/memlog.py enforces -- 5 KiB of headroom where there are
// 22 KiB today.
//
// Two things make that worse than it reads:
//
//  - Almost none of it is the host, so almost none of it can be tuned away.
//    lld_con 3,554 / lld_scan 3,122 / sch_prog 1,335 / rwble 1,141 /
//    lld_adv 982 / arch_main 848 are the *controller*, a precompiled library;
//    the NimBLE host's own .bss across every file above is under 200 bytes.
//    sdkconfig.ble is already central-only, one connection, no GATT server, no
//    bonding, no 5.0 features. This is the floor.
//  - It is the static half only. The controller and the host take their real
//    buffers from the heap at esp_bt_controller_init()/enable() and
//    nimble_port_init(), plus a 4 KiB host task stack, and that is the number
//    this board actually lives or dies by -- the largest contiguous free block
//    while an app is up measures about 23.5 KiB, and the Rust UI core aborts on
//    a failed allocation instead of returning an error, so overcommitting
//    reboots the device rather than raising. That figure needs the board and
//    has not been taken.
//
// So section 12 stays unimplemented until sections 14 and 17.8 are satisfied:
// a wireless profile measured on hardware, with RAM minimum, largest
// contiguous block, stack and FPS recorded per profile. sdkconfig.ble and
// .cache/blemeasure/ reproduce the numbers above without touching any other
// build directory.
//
// Everything here runs on the JS owner task. There is no pump and no reset:
// nothing is started, so nothing can be left running.

esp_err_t pocket_ble_install(JSContext *ctx, void *user_data);
