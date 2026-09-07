#pragma once
#include "esp_err.h"
#include "quickjs.h"

// pocket.io -- the external interfaces of docs/common-api.md section 10, plus
// io.ir from section 9.
//
// Everything here runs on the JS owner task, inside app_tick()'s turn. That is
// not a convenience, it is the whole safety argument, so it is written out
// below in full.
//
// ------------------------------------------------------------ the shared bus
//
// board_init() creates one I2C master bus on GPIO8/9 and puts three devices the
// firmware cannot lose on it: the TCA8418 keyboard at 0x34, the BMI270 at 0x68
// or 0x69 (motion.c probes both), and the ES8311 codec at 0x18. input_task
// polls the keyboard and the IMU over that bus every 5 ms at priority 6; the JS
// task draws and runs the guest at priority 5. Section 10 of the document lets
// a program reach that same bus through the EXT connector, whose I2C pins ARE
// GPIO8/9 -- there is no second internal bus to give it. So the guest gets the
// real one, with four guards, and each guard is published in the io.i2c
// capability's limits rather than living only in this comment:
//
//   1. Reserved addresses are refused at open(), with PERMISSION_DENIED. The
//      list is 0x18, 0x34, 0x68 and 0x69 -- the three drivers above, both IMU
//      addresses because motion.c will take either -- plus I2C's own reserved
//      ranges 0x00-0x07 and 0x78-0x7F, which are not devices at all. The list
//      is published as `reservedAddresses`. A refused address is refused for
//      the life of the session; there is no override.
//   2. The bus is entered only from the JS task, only through IDF's own
//      i2c_master_transmit/_receive, and only for the length of one call. The
//      driver holds a per-bus mutex for a transaction, so a guest transfer is
//      serialised against input_task's polls rather than racing them, and
//      because input_task is the higher-priority waiter, priority inheritance
//      pushes the guest's transfer through instead of leaving the keyboard
//      behind a lower-priority task. No worker task, no timer and no ISR in
//      this file ever touches the bus.
//   3. Every transfer is bounded in length -- 256 bytes each direction, the
//      section 14 figure -- and in time, by the transaction timeout the call
//      passes to the driver. That timeout covers the wait for the bus as well
//      as the transfer, so it is the whole of what the keyboard can be made to
//      wait: 50 ms on the shared bus, published as `maxWaitMs`. The Grove port
//      is its own bus with nothing else on it and is allowed 100 ms.
//   4. There is no scan. Section 10 asks for validated port names and pin sets
//      rather than raw addresses to sweep, so io.ports() enumerates the ports
//      and a program probes only the address it opened.
//
// The other buses are simpler because nothing in the firmware shares them:
// SPI3 (EXT/microSD pins, and no SD driver exists in this build), UART1 on
// GPIO13/15, three EXT GPIOs, and RMT on GPIO44. GPIO8/9 are never offered as
// GPIO ports, and neither are the LCD, flash, USB, power and strapping pins --
// section 10 reserves those by not publishing them.
//
// ---------------------------------------------------------------- io.ir
//
// The board does have an IR emitter. M5Stack's official Cardputer-Adv pin map
// lists "G44 -> IR TX", docs/hardware-constraints.md records the same GPIO from
// the same source, and GPIO44 is free here because the console is USB
// Serial/JTAG (CONFIG_ESP_CONSOLE_UART_NUM=-1), so UART0's pins are unused.
// There is no receiver: nothing in the pin map, the product page or M5Unified
// mentions one, and section 9 says outright that reception is not to be treated
// as fitted. io.ir therefore sends and has no receive method at all.

// Registers pocket.io. Pass to pocketjs_guest_quickjs_install_once() after
// pocket_api_install().
esp_err_t pocket_io_install(JSContext *ctx, void *user_data);

// Polls the UART's receive ring and the GPIO watches, and posts the completions
// the two of them produce. Call once per frame from the JS task, immediately
// before pocket_api_pump() so a read that came due settles in the same turn.
void pocket_io_pump(void);

// Closes every handle, returns every pin this file drove to a floating input,
// and gives the buses back. Call from app_stop() while the guest is still
// alive, next to the other surfaces' resets.
void pocket_io_reset(void);
