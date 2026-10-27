#include "usb_console.h"
#include "esp_vfs.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

static esp_vfs_t vfs;
static char wire[32768];
static size_t used, packets[1024];
static unsigned calls, fail_call, stream_count, buffering_count;
static int64_t now;
static bool deny_lock, nested_bridge;
int64_t esp_timer_get_time(void) { return now; }
bool usb_serial_jtag_is_driver_installed(void) { return true; }
SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) { return s; }
int xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks) {
    assert(ticks<=50);
    if(deny_lock || s->held) return 0;
    s->held=1;return 1;
}
void xSemaphoreGive(SemaphoreHandle_t s) { assert(s->held);s->held=0; }
int usb_serial_jtag_write_bytes(const void *p, size_t n, uint32_t ticks) {
    assert(ticks>0 && ticks<=50 && calls<1024);
    packets[calls++]=n;
    if(nested_bridge) {
        nested_bridge=false;
        assert(usb_console_frame("\x1d" "B00\n",5)==ESP_ERR_TIMEOUT);
    }
    if(calls==fail_call) { now+=2*ticks*1000;return 0; }
    assert(used+n<sizeof(wire));
    memcpy(wire+used,p,n);used+=n;wire[used]=0;
    now+=1000;return (int)n;
}
esp_err_t esp_vfs_register(const char *path, const esp_vfs_t *p, void *ctx) {
    assert(!strcmp(path,"/dev/pocketusb") && ctx==NULL);vfs=*p;return ESP_OK;
}
FILE *host_freopen(const char *p, const char *mode, FILE *stream) {
    assert(!strcmp(mode,"w"));
    assert(!strcmp(p,stream==stdout?"/dev/pocketusb/out":"/dev/pocketusb/err"));
    stream_count++;return stream;
}
int host_setvbuf(FILE *stream, char *p, int mode, size_t size) {
    assert((stream==stdout||stream==stderr) && !p && mode==_IONBF && !size);
    buffering_count++;return 0;
}
static void clean(void) {
    assert(vfs.write(1,"\n",1)==1);
    used=calls=fail_call=0;now=0;wire[0]=0;
}
static void wrote(int fd, const char *s) { assert(vfs.write(fd,s,strlen(s))==(ssize_t)strlen(s)); }

// Actual main.c teardown, with guest/screen ports replaced by deterministic ones.
static bool running, frameless;
static int keys,owner,restored,pending_calls;
static const char *home_error;
static int run_started, ended_status;
static bool app_frameless(void) { return frameless; }
typedef struct {void (*run_restore)(void);void (*ended)(int,const char *);} screen_ops_t;
static screen_ops_t SCREENS[2];
static void app_stop(void) { wrote(1,"APP_STOPPED\n"); }
static void xQueueReset(int q) { (void)q; }
static const char *app_error(void) { return ""; }
static void sound_play(int n) { (void)n; }
static void take_pending_run(void) { pending_calls++; }
static void restore(void) { restored++; }
static void editor_ended(int status,const char *why) { ended_status=status;(void)why;wrote(1,"EDITOR_READY\n"); }
#define ESP_LOGI(tag,message) wrote(1,message "\n")
#include "end_run.inc"

int main(void) {
    assert(usb_console_init()==ESP_OK);
    assert(stream_count==2 && buffering_count==2);
    assert(vfs.open("/out",0,0)==1 && vfs.open("/err",0,0)==2);
    assert(vfs.open("/unknown",0,0)==-1);
    struct stat st;assert(!vfs.fstat(1,&st) && S_ISCHR(st.st_mode));
    assert(!vfs.close(1) && !vfs.write(1,"",0));
    assert(usb_console_init()==ESP_ERR_INVALID_STATE);
    clean();wrote(1,"prefix");wrote(1," continuation\n");
    assert(!strcmp(wire,"prefix continuation\n"));
    clean();wrote(1,"guest \x1d" "C0011\n");wrote(2,"\x1d" "B00\n");
    assert(!strcmp(wire,"guest \\x1dC0011\n\\x1dB00\n"));
    assert(memchr(wire,0x1d,used)==NULL);
    clean();wrote(1,"APP_STOPPED");fail_call=calls+1;
    assert(vfs.write(1,"\n",1)==-1 && errno==EAGAIN);
    wrote(1,"HOME_READY\n");
    assert(!strcmp(wire,"APP_STOPPED\nHOME_READY\n"));
    clean();char long_line[600];memset(long_line,'x',sizeof(long_line));
    fail_call=2;
    assert(vfs.write(1,long_line,sizeof(long_line))==256 && calls==2);
    wrote(1,"HOME_READY\n");
    assert(!strcmp(wire+256,"\nHOME_READY\n"));
    clean();wrote(1,"stdout prefix");wrote(2,"stderr\n");wrote(1,"HOME_READY\n");
    assert(!strcmp(wire,"stdout prefix\nstderr\nHOME_READY\n"));
    clean();wrote(1,"prefix");
    char frame[995];memset(frame,'a',sizeof(frame));frame[0]=0x1d;frame[1]='B';frame[994]='\n';
    unsigned before=calls;
    assert(usb_console_frame(frame,sizeof(frame))==ESP_OK);
    assert(calls==before+2 && packets[calls-1]==995 && wire[6]=='\n');
    assert(!memcmp(wire+7,frame,sizeof(frame)));
    wrote(2,"HOME_READY\n");assert(!strcmp(wire+1002,"HOME_READY\n"));
    clean();fail_call=1;
    assert(usb_console_frame(frame,sizeof(frame))==ESP_ERR_TIMEOUT && used==0 && calls==1);
    assert(now<=100000);
    wrote(1,"HOME_READY\n");assert(!strcmp(wire,"\nHOME_READY\n"));
    assert(usb_console_frame(NULL,0)==ESP_ERR_INVALID_SIZE);
    assert(usb_console_frame("bad",3)==ESP_ERR_INVALID_SIZE);
    clean();deny_lock=true;
    assert(vfs.write(1,"lost\n",5)==-1 && used==0 && calls==0);
    deny_lock=false;wrote(1,"HOME_READY\n");assert(!strcmp(wire,"\nHOME_READY\n"));
    clean();nested_bridge=true;wrote(1,"stdout\n");
    wrote(2,"HOME_READY\n");assert(!strcmp(wire,"stdout\n\nHOME_READY\n"));
    clean();char huge[16384];memset(huge,'z',sizeof(huge));
    ssize_t n=vfs.write(1,huge,sizeof(huge));
    assert(n>0 && n<(ssize_t)sizeof(huge) && now<=100000 && calls<=50);
    clean();owner=0;running=true;SCREENS[0].run_restore=restore;
    end_run(ESP_OK);
    assert(!running && restored==1 && pending_calls==1);
    assert(!strcmp(wire,"APP_STOPPED\nHOME_READY\n"));
    clean();owner=1;running=true;SCREENS[1].ended=editor_ended;
    end_run(ESP_OK);
    assert(!strcmp(wire,"APP_STOPPED\nEDITOR_READY\n") && !strstr(wire,"HOME_READY"));
    assert(ended_status==ESP_OK);
    clean();running=true;frameless=true;run_started=ESP_OK;
    end_run(ESP_OK);
    assert(ended_status==ESP_ERR_NOT_FOUND);
    assert(!strcmp(wire,"APP_STOPPED\nEDITOR_READY\n"));
    clean();running=true;run_started=ESP_ERR_TIMEOUT;
    end_run(ESP_OK);
    assert(ended_status==ESP_ERR_TIMEOUT);
    assert(!strcmp(wire,"APP_STOPPED\nEDITOR_READY\n"));
    puts("USB_CONSOLE_HOME PASS: finite refusal, recovery LF, streams, bridge, actual end_run");
}
