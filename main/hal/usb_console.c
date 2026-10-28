#include "usb_console.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

// IDF 6.0.1's default USB VFS returns size even when tx_char drops a byte.
// The driver queues a block whole or refuses it. Its two waits may each use
// the remaining budget: at most 100 ms plus tick rounding for the whole call,
// including our lock, even for long output or a disconnected host.
#define TX_BUDGET_US 50000
#define TX_CHUNK 256
#define CONSOLE_PATH "/dev/pocketusb"
static StaticSemaphore_t lock_storage;
static SemaphoreHandle_t tx_lock;
static atomic_bool broken_line;
static bool line_open;
static int last_stream;

static TickType_t remaining(int64_t deadline) {
    int64_t us=deadline-esp_timer_get_time();
    return us<=0 ? 0 : (TickType_t)((us+1000*portTICK_PERIOD_MS-1)/(1000*portTICK_PERIOD_MS));
}
static bool send_bytes(const void *data, size_t size, int64_t deadline) {
    TickType_t ticks=remaining(deadline);
    return ticks && usb_serial_jtag_write_bytes(data,size,ticks)==(int)size;
}
static ssize_t transmit(int stream, const void *data, size_t size, bool frame) {
    if(!size) return 0;
    if(!tx_lock) { errno=ENODEV;return -1; }
    const int64_t deadline=esp_timer_get_time()+TX_BUDGET_US;
    if(xSemaphoreTake(tx_lock,remaining(deadline))!=pdTRUE) {
        atomic_store(&broken_line,true);
        errno=EAGAIN;return -1;
    }
    const unsigned char *bytes=data;
    size_t done=0;
    // A refused write may contain LF. Never join the next record to its
    // surviving prefix. Switching stdout/stderr/bridge ends a partial record;
    // fragments on the same healthy stream may still compose normally.
    bool repair=atomic_exchange(&broken_line,false);
    bool boundary=repair || (line_open && stream!=last_stream);
    if(boundary && !send_bytes("\n",1,deadline)) goto refused;
    if(boundary) line_open=false;
    while(done<size) {
        size_t n=size-done;
        if(!frame && n>TX_CHUNK) n=TX_CHUNK;
        if(!frame) {
            /* Console strings are untrusted observations. Only the frame
             * writer may emit GS: printing a valid CRC cannot forge an ACK. */
            const unsigned char *gs=memchr(bytes+done,0x1d,n);
            if(gs==bytes+done) {
                if(!send_bytes("\\x1d",4,deadline))goto refused;
                done++;last_stream=stream;line_open=true;continue;
            }
            if(gs)n=(size_t)(gs-(bytes+done));
        }
        if(!send_bytes(bytes+done,n,deadline)) goto refused;
        done+=n;
        last_stream=stream;
        line_open=bytes[done-1]!='\n';
    }
    xSemaphoreGive(tx_lock);
    return (ssize_t)done;
refused:
    // No hidden retries. Report only queued source bytes, not the repair LF.
    atomic_store(&broken_line,true);
    xSemaphoreGive(tx_lock);
    errno=EAGAIN;
    return done ? (ssize_t)done : -1;
}
static ssize_t console_write(int fd, const void *data, size_t size) {
    return transmit(fd,data,size,false);
}
static int console_open(const char *path, int flags, int mode) {
    (void)flags;(void)mode;
    if(!strcmp(path,"/out")) return 1;
    if(!strcmp(path,"/err")) return 2;
    errno=ENOENT;return -1;
}
static int console_close(int fd) { (void)fd;return 0; }
static int console_stat(int fd, struct stat *st) {
    (void)fd;memset(st,0,sizeof(*st));st->st_mode=S_IFCHR;return 0;
}
esp_err_t usb_console_init(void) {
    if(tx_lock || !usb_serial_jtag_is_driver_installed()) return ESP_ERR_INVALID_STATE;
    tx_lock=xSemaphoreCreateMutexStatic(&lock_storage);
    if(!tx_lock) return ESP_ERR_NO_MEM;
    const esp_vfs_t vfs={.flags=ESP_VFS_FLAG_DEFAULT,.write=console_write,
                        .open=console_open,.close=console_close,.fstat=console_stat};
    esp_err_t err=esp_vfs_register(CONSOLE_PATH,&vfs,NULL);
    if(err!=ESP_OK) return err;
    if(!freopen(CONSOLE_PATH "/out","w",stdout) ||
       !freopen(CONSOLE_PATH "/err","w",stderr)) return ESP_FAIL;
    // Separate buffered tails must not be reordered across streams or bridge.
    if(setvbuf(stdout,NULL,_IONBF,0) || setvbuf(stderr,NULL,_IONBF,0)) return ESP_FAIL;
    return ESP_OK;
}
esp_err_t usb_console_frame(const void *data, size_t size) {
    if(!data || !size || size>1024 || ((const char *)data)[size-1]!='\n')
        return ESP_ERR_INVALID_SIZE;
    return transmit(3,data,size,true)==(ssize_t)size ? ESP_OK : ESP_ERR_TIMEOUT;
}
