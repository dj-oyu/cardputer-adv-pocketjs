#include "board.h"
#include "shell.h"
#include "motion.h"
#include "sound.h"
#include "keymap.h"
#include "editor.h"
#include "codeedit.h"
#include "jpfont.h"
#include "skk_session.h"
#include "app_session.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include <stdatomic.h>

static QueueHandle_t keys;
static atomic_bool stop;
static atomic_bool capture;
static atomic_int diagnostic;
static atomic_bool editing;   // SKK practice
static atomic_bool coding;    // Playground

// USB drives the shell with single letters, but the editor needs the bytes
// themselves so a host script can type romaji at it. The mode decides which
// reading applies; 0x1b closes the editor either way.
static bool usb_stroke(char c, keystroke_t *k) {
    memset(k,0,sizeof(*k));
    // Every printable byte belongs to the editor while it is open — 's' and 'c'
    // included — so the host controls move to control bytes there.
    if(atomic_load(&editing)||atomic_load(&coding)) {
        // C-s is the Playground's save, so the host capture moves to C-p.
        if(c==0x10) { atomic_store(&capture,true); return false; }   // C-p
        if(c==27) { k->text[0]='\0';memcpy(k->text+1,"esc",3);k->len=4;k->nav=KEY_BACK;return true; }
        if(c=='\r'||c=='\n') { k->text[0]='\n';k->len=1;k->nav=KEY_ENTER;return true; }
        if(c=='\b'||c==0x7f) { k->text[0]='\b';k->len=1;return true; }
        if(c==0x0b) { k->toggle_ime=true;return true; }   // C-k stands in for C-j
        if((unsigned char)c<0x20) { k->text[0]=c;k->len=1;return true; }
        k->text[0]=c;k->len=1;return true;
    }
    if(c=='s') { atomic_store(&capture,true); return false; }
    if(c=='c') { motion_recenter(); return false; }
    if(c>='1'&&c<='6') { atomic_store(&diagnostic,c); return false; }
    if(c=='\r'||c=='\n'||c=='e')k->nav=KEY_ENTER;
    else if(c=='q'||c==27)k->nav=KEY_BACK;
    else if(c=='b')k->nav=KEY_RIGHT;
    else if(c=='a')k->nav=KEY_LEFT;
    else if(c=='u')k->nav=KEY_UP;
    else if(c=='d')k->nav=KEY_DOWN;
    else return false;
    return true;
}

static void input_task(void *arg) {
    (void)arg;
    while(1) {
        keystroke_t k;
        bool have=keymap_poll(&k);
        motion_poll();
        char c;
        if(!have && usb_serial_jtag_read_bytes(&c,1,0)>0) have=usb_stroke(c,&k);
        if(have) {
            bool typing=atomic_load(&editing)||atomic_load(&coding);
            if(k.force_stop || (k.nav==KEY_BACK && !typing)) {
                atomic_store(&stop,true);app_request_stop();
            } else xQueueSend(keys,&k,0);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
static void ui_task(void *arg) {
    (void)arg;
    bool running=false;
    const char *error=NULL;
    unsigned phase=0;
    ESP_LOGI("shell","HOME_READY");
    while(1) {
        int64_t frame_start=esp_timer_get_time();
        keystroke_t stroke={0};
        bool have=xQueueReceive(keys,&stroke,0)==pdTRUE;
        board_key_t key=have?stroke.nav:KEY_NONE;

        if(atomic_load(&editing)) {
            if(have && !editor_key(&stroke)) {
                atomic_store(&editing,false);
                sound_play(2);
                ESP_LOGI("shell","HOME_READY");
            } else {
                bool shot=atomic_exchange(&capture,false);
                if(shot||editor_dirty()) {
                    if(shot)board_capture(true);
                    editor_draw();
                    if(shot)board_capture(false);
                    // The frame cap would hide what a repaint actually costs,
                    // so the work is timed apart from the wait.
                    if(!shot) {
                        int64_t work=esp_timer_get_time()-frame_start;
                        static int64_t sum, peak; static unsigned n;
                        sum+=work; if(work>peak)peak=work; n++;
                        if(n==8) {
                            ESP_LOGI("editor","REPAINT avg=%lldus max=%lldus",sum/8,peak);
                            sum=0;peak=0;n=0;
                        }
                    }
                }
                int held=(int)((esp_timer_get_time()-frame_start)/1000);
                vTaskDelay(pdMS_TO_TICKS(held<16?16-held:1));
                continue;
            }
        }

        if(atomic_load(&coding)) {
            if(code_state()==CODE_EDIT) {
                if(have && !code_key(&stroke)) {
                    atomic_store(&coding,false);
                    sound_play(2);
                    ESP_LOGI("shell","HOME_READY");
                } else {
                    // C-r moved the Playground into its run; start the guest on
                    // the source it just saved.
                    if(code_state()==CODE_RUNNING) {
                        size_t n=0;
                        const char *src=code_source(&n);
                        esp_err_t e=app_start_source(src,n);
                        if(e==ESP_OK) running=true;
                        else if(e==ESP_ERR_NOT_FOUND) {
                            // The source drew whatever it drew and defined no
                            // frame; there is nothing to run, so show what it
                            // printed instead of reporting a failure.
                            app_stop(); running=false;
                            code_returned("EVALUATED (NO frame)");
                        } else {
                            running=false;
                            const char *why=app_error();
                            code_returned(why[0]?why:"START FAILED");
                        }
                        ESP_LOGI("code","RUN %u bytes -> %s",(unsigned)n,esp_err_to_name(e));
                    } else {
                        bool shot=atomic_exchange(&capture,false);
                        if(shot||code_dirty()) {
                            if(shot)board_capture(true);
                            code_draw();
                            if(shot)board_capture(false);
                            if(!shot) {
                                int64_t work=esp_timer_get_time()-frame_start;
                                static int64_t sum, peak; static unsigned n;
                                sum+=work; if(work>peak)peak=work; n++;
                                if(n==8) {
                                    ESP_LOGI("code","REPAINT avg=%lldus max=%lldus",sum/8,peak);
                                    sum=0;peak=0;n=0;
                                }
                            }
                        }
                    }
                    int held=(int)((esp_timer_get_time()-frame_start)/1000);
                    vTaskDelay(pdMS_TO_TICKS(held<16?16-held:1));
                    continue;
                }
            } else {
                // Running the Playground's source. Back returns to the editor
                // rather than all the way to the home screen.
                // Back leaves the run for the editor. The stroke arrives as a
                // normal key here (input_task does not turn it into a stop
                // while the Playground owns the keyboard), so it is read here.
                bool leave=have && stroke.nav==KEY_BACK;
                if(leave) app_request_stop();
                if(leave || atomic_exchange(&stop,false) || !running) {
                    if(running)app_stop();
                    running=false;
                    code_returned(error);
                    error=NULL;
                    xQueueReset(keys);
                    sound_play(2);
                } else {
                    bool shot=atomic_exchange(&capture,false);
                    if(shot) {board_capture(true);app_force_redraw();}
                    if(key==KEY_ENTER)sound_play(1);
                    esp_err_t e=app_tick(key==KEY_ENTER?0x4000:0);
                    if(e==ESP_OK && key==KEY_ENTER)e=app_tick(0);
                    if(shot)board_capture(false);
                    if(e!=ESP_OK) {
                        app_stop();running=false;
                        const char *why=app_error();
                        code_returned(why[0]?why:"EXECUTION FAILED");
                    }
                }
                int held=(int)((esp_timer_get_time()-frame_start)/1000);
                vTaskDelay(pdMS_TO_TICKS(held<33?33-held:1));
                continue;
            }
        }

        if(atomic_exchange(&stop,false)) {
            if(running||error) {
                sound_play(2);
                if(running)app_stop();
                ESP_LOGI("shell","HOME_READY");
            } else shell_key(KEY_BACK);
            running=false;error=NULL;xQueueReset(keys);
        } else if(!running && key!=KEY_NONE) {
            if(error&&key==KEY_ENTER)error=NULL;
            else if(!error&&shell_key(key)) {
                switch(shell_app()) {
                    case 1:
                        editor_open();atomic_store(&editing,true);
                        ESP_LOGI("editor","EDITOR_READY");
                        break;
                    case 2:
                        code_open();atomic_store(&coding,true);
                        ESP_LOGI("code","CODE_READY");
                        break;
                    default:
                        running=app_start()==ESP_OK;
                        if(!running)error="START FAILED";
                }
            }
            key=KEY_NONE;
        }
        int test=atomic_exchange(&diagnostic,0);
        if(test && !running) {
            running=app_start_test(test)==ESP_OK;
            if(!running)error="TEST ERROR";
        }
        bool snapshot=atomic_exchange(&capture,false);
        if(snapshot) {board_capture(true);app_force_redraw();}
        if(running) {
            if(key==KEY_ENTER)sound_play(1);
            esp_err_t e=app_tick(key==KEY_ENTER?0x4000:0);
            // Insert a release frame so consecutive queued presses remain distinct.
            if(e==ESP_OK && key==KEY_ENTER)e=app_tick(0);
            if(e!=ESP_OK) {app_stop();running=false;error="EXECUTION FAILED";}
        } else shell_draw(error,phase++);
        if(snapshot)board_capture(false);
        int elapsed_ms=(int)((esp_timer_get_time()-frame_start)/1000);
        vTaskDelay(pdMS_TO_TICKS(elapsed_ms<33?33-elapsed_ms:1));
    }
}
void app_main(void) {
    ESP_LOGI("boot","Cardputer ADV PocketJS M1; app=3MiB skk=2MiB fonts=512KiB");
    ESP_ERROR_CHECK(board_init());
    shell_init();
    // Neither is fatal: the home stays usable with no dictionary and no font,
    // and the editor shows which one is missing.
    jpfont_init();
    skk_session_init();
    usb_serial_jtag_driver_config_t usb={.tx_buffer_size=1024,.rx_buffer_size=256};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    keys=xQueueCreate(16,sizeof(keystroke_t));configASSERT(keys);
    configASSERT(xTaskCreate(input_task,"input",4096,NULL,6,NULL)==pdPASS);
    configASSERT(xTaskCreate(ui_task,"ui",32768,NULL,5,NULL)==pdPASS);
}
