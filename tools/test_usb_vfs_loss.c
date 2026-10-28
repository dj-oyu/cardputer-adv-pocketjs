// Compile the pinned IDF functions themselves, not a rewritten loss model.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#define TX_FLUSH_TIMEOUT_US 50000
#define ESP_LINE_ENDINGS_LF 0
#define ESP_LINE_ENDINGS_CR 1
static struct {int write_lock,tx_mode;int64_t last_tx_ts;void (*tx_func)(int,int);} s_ctx;
static char wire[128];static size_t used,capacity;
static int64_t now;
static int64_t esp_timer_get_time(void) { now+=1000;return now; }
static bool usb_serial_jtag_ll_txfifo_writable(void) { return used<capacity; }
static void usb_serial_jtag_ll_write_txfifo(const uint8_t *p,size_t n) { memcpy(wire+used,p,n);used+=n; }
static void usb_serial_jtag_ll_txfifo_flush(void) {}
static bool usb_serial_jtag_is_connected(void) { return true; }
static void _lock_acquire_recursive(int *p) { (void)p; }
static void _lock_release_recursive(int *p) { (void)p; }
#include "idf_vfs.inc"
int main(void) {
    s_ctx.tx_func=usb_serial_jtag_tx_char_no_driver;
    capacity=7;
    assert(usb_serial_jtag_write(NULL,1,"partial\n",8)==8);
    assert(used==7 && !memcmp(wire,"partial",7));
    capacity=sizeof(wire);
    assert(usb_serial_jtag_write(NULL,1,"HOME_READY\n",11)==11);
    assert(used==18 && !memcmp(wire,"partialHOME_READY\n",18));
    puts("IDF601_VFS_LOSS REPRODUCED: accepted 8 bytes, lost LF, next HOME line joined");
}
