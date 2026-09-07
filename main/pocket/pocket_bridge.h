#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "quickjs.h"

// pocket.bridge -- the PC link of docs/common-api.md section 13.
//
// There is no second wire on this board. The bridge rides the USB Serial/JTAG
// byte stream that already carries the console and ESP_LOGI, so the whole
// design is about telling a frame apart from a log line in both directions.
//
//     0x1d 'B' <hex of body and CRC32> '\n'
//
// 0x1d (ASCII GS) is the trick. Log output is printable UTF-8, CR, LF and the
// occasional 0x1b colour escape; it never contains a Group Separator. A PC
// reader therefore scans the RAW byte stream -- not lines -- for 0x1d, accepts
// only [0-9a-f] until the LF, and abandons the frame on anything else. The
// CRC32 is the second net, and resync is always "wait for the next 0x1d".
//
// Going out, a frame is written with a single usb_serial_jtag_write_bytes()
// call. The driver serialises whole calls on its tx_mux and hands the item to
// its TX ring all or nothing, so a log line cannot be spliced into the middle
// of a frame -- provided the frame fits the ring. That is where the size below
// comes from, and it is the reason this transport does not take section 13's
// proposed 1024: the ring is 1024 bytes (main.c), hex doubles everything, and a
// 1024-byte frame would need 2051 bytes on the wire and be split by definition.
// A log line CAN still be split around a frame, which is why a reader that
// wants log text must reassemble it rather than trust one readline().
//
// pet_hub.c's fixed 48-byte frame starts with 0x1e and is untouched. Its parser
// returns false for 0x1d while it is idle, so main.c can offer every byte here
// first and everything that is not ours still reaches the old path unchanged.
//
// Body layout, little endian:
//
//      0  u8   version, always 1
//      1  u8   kind, one of the constants below
//      2  u16  payload bytes
//      4  u32  sessionId -- a frame from an ended session matches nothing
//      8  u32  requestId -- 0 on an event, where the field carries a sequence
//     12  ...  payload
//   12+n  u32  CRC32 (the usual reflected 0xedb88320 one) over bytes 0..12+n

#define POCKET_BRIDGE_VERSION 1
#define POCKET_BRIDGE_HEADER  12
// Header + payload + CRC. See above: 2 + 2*496 + 1 = 995 wire bytes, which is
// the largest frame that still fits the firmware's 1024-byte USB TX ring whole.
#define POCKET_BRIDGE_MAX_FRAME   496
#define POCKET_BRIDGE_MAX_PAYLOAD (POCKET_BRIDGE_MAX_FRAME-POCKET_BRIDGE_HEADER-4)
#define POCKET_BRIDGE_MAX_WIRE    (2+2*POCKET_BRIDGE_MAX_FRAME+1)

// Device to PC.
#define POCKET_BRIDGE_HELLO    1   // payload: peerId '\0' apiVersion
#define POCKET_BRIDGE_REQUEST  3   // payload: method '\0' JSON
#define POCKET_BRIDGE_BYE      7   // payload: empty
// PC to device.
#define POCKET_BRIDGE_WELCOME  2   // payload: peerId echoed back
#define POCKET_BRIDGE_RESPONSE 4   // payload: JSON result
#define POCKET_BRIDGE_ERROR    5   // payload: code '\0' message
#define POCKET_BRIDGE_EVENT    6   // payload: topic '\0' JSON; requestId=sequence

// The exit. `body` is the header and payload of one frame, POCKET_BRIDGE_HEADER
// to POCKET_BRIDGE_MAX_FRAME-4 bytes; the CRC32, the hex and the delimiters are
// added here. Costs ~1 KiB of the caller's stack for the wire buffer, so call
// it from a task with room -- the UI task, which is the only caller today.
//
// ESP_ERR_INVALID_SIZE for a body outside that range, ESP_ERR_INVALID_STATE
// when the USB driver is not installed, ESP_ERR_TIMEOUT when the TX ring stayed
// full (nothing is written in that case: the driver's send is all or nothing).
esp_err_t pocket_bridge_emit(const uint8_t *body, size_t len);

// The entrance, called from main.c's USB reading with every byte. Returns true
// when the byte belonged to a bridge frame and must not become a keystroke.
// Runs on the input task.
bool pocket_bridge_usb(uint8_t byte);

// pocket.bridge.connect(). Pass to pocketjs_guest_quickjs_install_once().
esp_err_t pocket_bridge_install(JSContext *ctx, void *user_data);

// Settles what the PC answered and delivers its events. JS owner task, once per
// frame, before pocket_api_pump() so a reply lands in the same turn.
void pocket_bridge_pump(void);

// Drops the session and its listeners while the guest is still alive. The calls
// in flight belong to pocket_api_reset(), which runs after this.
void pocket_bridge_reset(void);
