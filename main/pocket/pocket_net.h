#pragma once
#include "esp_err.h"
#include "quickjs.h"

// pocket.net — section 11 of docs/common-api.md: a Wi-Fi lease and HTTP over it.
//
// The radio on this board is not a service that stays up. Linking it costs
// about 37 KiB of DRAM, running it costs a further 4.8 KiB that
// esp_netif_deinit() cannot give back on IDF v6.0.1, and continuous RX draws
// 88 mA against a modem-sleep baseline of 51-66. wifi_time.c was written around
// that: it brings the radio up for one SNTP exchange and takes it down again.
//
// So this surface does not own the radio either. It borrows it for the length
// of a lease the app opens and closes -- pocket.net.wifi.acquire() and
// WifiLease.close() -- through the same radio_up()/tear_down() pair and the
// same single-attempt lock wifi_time.c already uses for the clock and the SSID
// scan. A lease and a clock sync therefore cannot overlap: whichever asks
// second is told BUSY. The lease model rather than a per-request one because
// association is the expensive part in *time* (seconds), so a per-request radio
// would put a multi-second bring-up in front of every call and hide the DRAM
// cost inside it. An explicit lease makes the app say when it pays.
//
// Everything here runs on the JS owner task except the HTTP worker, which is a
// task of its own that touches no JS value and hands its results back through
// pocket_api_complete(). pocket_net_pump() is what turns a link state change
// and a finished scan into a settled Promise, and must be called once per frame
// from the JS task.

esp_err_t pocket_net_install(JSContext *ctx, void *user_data);

// Follows the link and the scan, settling what they finished. Cheap when
// nothing is in flight: one load and one branch.
void pocket_net_pump(void);

// Drops the lease and asks a request in flight to stop. Call from the JS task
// while the guest is still alive -- app_stop() before it destroys the guest.
// Non-blocking: the HTTP worker owns the socket and takes the radio down itself
// once it has let go of it, so a program that exits mid-request costs the frame
// nothing. A new session that asks for a lease before that finishes gets BUSY,
// which is retryable and honest.
void pocket_net_reset(void);
