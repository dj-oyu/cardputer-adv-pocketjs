#include "sound.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include <math.h>
#include <stdatomic.h>

static i2s_chan_handle_t output;
static QueueHandle_t events;
static atomic_bool enabled=true;
void sound_set_enabled(bool value){atomic_store(&enabled,value);}
void sound_play(int kind){if(events&&atomic_load(&enabled))xQueueSend(events,&kind,0);}
static void audio_task(void *arg) {
    (void)arg;int kind;int16_t pcm[256];
    while(1) {
        xQueueReceive(events,&kind,portMAX_DELAY);
        if(!atomic_load(&enabled))continue;
        int frames=kind==1?1440:kind==2?1080:720;
        float phase=0,frequency=kind==1?880:kind==2?440:660;
        for(int start=0;start<frames+256;start+=128) {
            for(int j=0;j<128;j++) {
                int n=start+j;float sample=0;
                if(n<frames && atomic_load(&enabled)) {
                    float u=(float)n/frames;
                    float envelope=fminf(n/72.0f,1.0f)*(1-u)*(1-u);
                    phase+=6.2831853f*frequency*(kind==1?1+0.35f*u:1-0.15f*u)/24000;
                    sample=1600*envelope*(sinf(phase)+0.18f*sinf(phase*2));
                }
                pcm[j*2]=pcm[j*2+1]=(int16_t)sample;
            }
            size_t written=0;
            esp_err_t err=i2s_channel_write(output,pcm,sizeof(pcm),&written,100);
            if(err!=ESP_OK||written!=sizeof(pcm)){ESP_LOGW("sound","I2S write failed");break;}
        }
        ESP_LOGI("sound","SFX %d synthesized",kind);
    }
}
void sound_init(i2c_master_bus_handle_t bus) {
    if(i2c_master_probe(bus,0x18,30)!=ESP_OK){ESP_LOGW("sound","ES8311 unavailable");return;}
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);
    channel.dma_desc_num=4;channel.dma_frame_num=128;channel.auto_clear=true;
    esp_err_t err=i2s_new_channel(&channel,&output,NULL);if(err!=ESP_OK)return;
    i2s_std_config_t cfg={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(24000),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=41,.ws=43,.dout=42,.din=I2S_GPIO_UNUSED}};
    err=i2s_channel_init_std_mode(output,&cfg);if(err!=ESP_OK)goto fail;
    err=i2s_channel_enable(output);if(err!=ESP_OK)goto fail;
    i2c_master_dev_handle_t codec;
    i2c_device_config_t dev={.dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=0x18,.scl_speed_hz=100000};
    err=i2c_master_bus_add_device(bus,&dev,&codec);if(err!=ESP_OK)goto fail;
    // Cardputer ADV ES8311: BCLK clock source, 16-bit I2S DAC, 0 dB.
    // Register values verified against M5Unified's Cardputer ADV callback.
    const uint8_t config[][2]={{0x00,0x80},{0x01,0xb5},{0x02,0x18},{0x0d,0x01},
        {0x12,0x00},{0x13,0x10},{0x32,0xbf},{0x37,0x08}};
    for(unsigned i=0;i<sizeof(config)/sizeof(config[0]);i++) {
        err=i2c_master_transmit(codec,config[i],2,30);if(err!=ESP_OK)break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    i2c_master_bus_rm_device(codec);if(err!=ESP_OK)goto fail;
    events=xQueueCreate(4,sizeof(int));
    if(!events)goto fail;
    if(xTaskCreate(audio_task,"sfx",4096,NULL,7,NULL)!=pdPASS){vQueueDelete(events);events=NULL;goto fail;}
    ESP_LOGI("sound","ES8311 ready; synthesized 24kHz stereo; default ON");return;
fail:
    ESP_LOGW("sound","Audio unavailable: %s",esp_err_to_name(err));
    i2s_channel_disable(output);i2s_del_channel(output);output=NULL;
}
