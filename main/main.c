#include "board.h"
#include "shell.h"
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
static void input_task(void *arg) {
    (void)arg;
    while(1) {
        board_key_t key=board_key();
        char c;
        if(usb_serial_jtag_read_bytes(&c,1,0)>0) {
            if(c=='\r'||c=='\n'||c=='e')key=KEY_ENTER;
            if(c=='q'||c==27)key=KEY_BACK;
            if(c=='b')key=KEY_RIGHT;
            if(c=='a')key=KEY_LEFT;
            if(c=='s')atomic_store(&capture,true);
            if(c>='1'&&c<='6')atomic_store(&diagnostic,c);
        }
        if(key==KEY_BACK) { atomic_store(&stop,true);app_request_stop(); }
        else if(key!=KEY_NONE) xQueueSend(keys,&key,0);
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
        board_key_t key=KEY_NONE;xQueueReceive(keys,&key,0);
        if(!running && (key==KEY_LEFT||key==KEY_RIGHT))shell_change_background(key==KEY_RIGHT?1:-1);
        if(atomic_exchange(&stop,false)) {
            if(running)app_stop();
            running=false;error=NULL;xQueueReset(keys);
            ESP_LOGI("shell","HOME_READY");
        } else if(!running && key==KEY_ENTER) {
            if(error)error=NULL;
            else { running=app_start()==ESP_OK;if(!running)error="START FAILED"; }
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
    ESP_LOGI("boot","Cardputer ADV PocketJS M1; app=3MiB skk=2MiB fonts=2MiB");
    ESP_ERROR_CHECK(board_init());
    usb_serial_jtag_driver_config_t usb={.tx_buffer_size=1024,.rx_buffer_size=256};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    keys=xQueueCreate(16,sizeof(board_key_t));configASSERT(keys);
    configASSERT(xTaskCreate(input_task,"input",4096,NULL,6,NULL)==pdPASS);
    configASSERT(xTaskCreate(ui_task,"ui",32768,NULL,5,NULL)==pdPASS);
}
