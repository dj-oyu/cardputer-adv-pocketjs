#include "sound.h"
#include "driver/i2s_std.h"
#include "esp_cpu.h"
#include "esp_timer.h"
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

// The three clicks, rendered once at startup rather than every time they play.
//
// Synthesising one of these cost 25.6 ms of CPU, measured, against the 59.4 ms
// it takes to play: two sinf, two float divisions and a float-to-int per sample
// came to about 3,600 cycles each. The audio task runs at priority 7 and shares
// core 0 with the drawing task (IDF pins an unpinned task to the first core on
// which it touches the FPU, and both of these use floats), so that 43% came
// straight out of the frames around it — about 7 ms of every frame that queued
// a sound. Playing from a table leaves the task with a copy to do.
//
// 6.5 KB of .bss for the three, mono; the stereo pair is made on the way out.
enum { SFX_KINDS=3, SFX_LONGEST=1440 };
static const int16_t sfx_frames[SFX_KINDS]={720,1440,1080};
static int16_t sfx_pcm[SFX_KINDS][SFX_LONGEST];

static void synthesize(int kind) {
    int frames=sfx_frames[kind];
    float phase=0,frequency=kind==1?880:kind==2?440:660;
    for(int n=0;n<frames;n++) {
        float u=(float)n/frames;
        float envelope=fminf(n/72.0f,1.0f)*(1-u)*(1-u);
        phase+=6.2831853f*frequency*(kind==1?1+0.35f*u:1-0.15f*u)/24000;
        // Peak stays near -7 dBFS including the second harmonic.
        sfx_pcm[kind][n]=(int16_t)(12000*envelope*(sinf(phase)+0.18f*sinf(phase*2)));
    }
}

static void audio_task(void *arg) {
    (void)arg;int kind;int16_t pcm[256];
    while(1) {
        xQueueReceive(events,&kind,portMAX_DELAY);
        if(!atomic_load(&enabled))continue;
        if(kind<0||kind>=SFX_KINDS)continue;
        int frames=sfx_frames[kind];
        // A tail of silence past the end pushes the last samples through the
        // DMA ring, as the synthesised version's frames+256 did.
        for(int start=0;start<frames+256;start+=128) {
            for(int j=0;j<128;j++) {
                int n=start+j;
                int16_t sample=(n<frames && atomic_load(&enabled))?sfx_pcm[kind][n]:0;
                pcm[j*2]=pcm[j*2+1]=sample;
            }
            size_t written=0;
            esp_err_t err=i2s_channel_write(output,pcm,sizeof(pcm),&written,100);
            if(err!=ESP_OK||written!=sizeof(pcm)){ESP_LOGW("sound","I2S write failed");break;}
        }
    }
}
void sound_init(i2c_master_bus_handle_t bus) {
    if(i2c_master_probe(bus,0x18,30)!=ESP_OK){ESP_LOGW("sound","ES8311 unavailable");return;}
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);
    channel.dma_desc_num=4;channel.dma_frame_num=128;channel.auto_clear=true;
    esp_err_t err=i2s_new_channel(&channel,&output,NULL);
    if(err!=ESP_OK){ESP_LOGW("sound","I2S channel unavailable: %s",esp_err_to_name(err));return;}
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
    int64_t began=esp_timer_get_time();
    for(int k=0;k<SFX_KINDS;k++)synthesize(k);
    ESP_LOGI("sound","3 clicks rendered in %lld us",esp_timer_get_time()-began);
    if(xTaskCreate(audio_task,"sfx",4096,NULL,7,NULL)!=pdPASS){vQueueDelete(events);events=NULL;goto fail;}
    ESP_LOGI("sound","ES8311 ready; synthesized 24kHz stereo; default ON");return;
fail:
    ESP_LOGW("sound","Audio unavailable: %s",esp_err_to_name(err));
    i2s_channel_disable(output);i2s_del_channel(output);output=NULL;
}
