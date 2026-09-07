#include "sound.h"
#include "ima_adpcm.h"
#include "driver/i2s_std.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
// sfx_pcm / sfx_offset / sfx_frames / wave, generated into the build tree by
// tools/make_sfx.py. The block above play_click says why they are not in .bss.
#include "sfx_tables.h"
// The synthesis those tables replace, kept so the board can check them.
#include "sfx_synth.h"
#include <stdatomic.h>
#include <stdlib.h>

#define SAMPLE_RATE ((int)SOUND_SAMPLE_RATE)

// A click, a tone, or a clip. kind is the click index, -1 for a tone, -2 for a
// clip; frequency is read only for tones, and the last three fields only for
// clips. One struct rather than a union because the queue is four entries deep:
// the twelve bytes a clip adds cost 48 bytes of .bss in total, and a union
// would cost the same reading twice as badly.
typedef struct {
    int32_t id;
    int16_t kind;
    uint16_t frequency;
    uint16_t gain;        // 0..4096
    uint32_t frames;
    sound_done_fn done;
    void *ctx;
    const uint8_t *data;  // clips: the caller's payload, read in place
    uint32_t bytes;
    uint16_t block;       // clips: ADPCM block size, 0 for PCM16
} request_t;

static i2s_chan_handle_t output;
static QueueHandle_t events;
static atomic_bool enabled=true;
static atomic_int cancelled;
static atomic_int next_id=1;
void sound_set_enabled(bool value){atomic_store(&enabled,value);}
bool sound_available(void){return events!=NULL;}
bool sound_play(int kind) {
    if(!events||!atomic_load(&enabled))return false;
    request_t req={.kind=(int16_t)kind};
    return xQueueSend(events,&req,0)==pdTRUE;
}
void sound_tone_cancel(int32_t id){if(id>0)atomic_store(&cancelled,id);}

// The three clicks and the tone's sine table live in flash, generated at build
// time by tools/make_sfx.py into sfx_tables.h.
//
// They used to be built into .bss at sound_init(): 8,640 B for a rectangular
// int16_t[3][1440] and 512 B for the wave, resident from boot to power-off.
// Sound is not a screen -- it plays on the home screen, in the editor and while
// an app runs -- so unlike the background scenes those bytes could never be
// released, and on a board with 341,760 B of DIRAM and no PSRAM they were 9 KB
// standing between an app and a TLS handshake. Nothing in either table depends
// on anything known at run time: not volume, which is applied per sample on the
// way out, and not mute, which is read per sample too. So they are const, which
// puts them in .rodata, which is flash reached through the cache, which is free.
//
// Why that is safe on an S3, where DMA cannot read flash-mapped memory: neither
// table ever reaches a DMA descriptor. play_click and play_tone read them a
// sample at a time with the CPU into pcm[], a local of audio_task and therefore
// internal RAM, and i2s_channel_write copies that into the driver's own DMA
// buffers. The DMA reads the driver's copy; the descriptors never see either of
// these pointers. Nor does an ISR: both are read only on the "sfx" task, so a
// flash write, which disables the cache, stalls that task rather than faulting
// it -- and this file's own code is flash-resident already, so an erase could
// delay a click before this change as much as after it.
//
// The rectangle is flattened. The real lengths are {720,1440,1080} = 3,240
// frames against the rectangle's 4,320, and sfx_offset[] costs one add at the
// one place that indexes it. sfx_frames[] still carries the per-kind length
// that the "SFX %d played %d frames" line prints, which tools/test_settings.py
// asserts on.
//
// What was measured, and why the tables exist at all: synthesising one click
// cost 25.6 ms of CPU against the 59.4 ms it takes to play -- two sinf, two
// float divisions and a float-to-int per sample, about 3,600 cycles each. The
// audio task runs at priority 7 and shares core 0 with the drawing task (IDF
// pins an unpinned task to the first core on which it touches the FPU, and both
// of these use floats), so that 43% came straight out of the frames around it,
// about 7 ms of every frame that queued a sound. Playing from a table leaves
// the task with a copy to do; playing from flash leaves it with the same copy.
//
// audio.tone takes any frequency and any length, so there is nothing to bake
// for it: the wave has to be produced while it plays. What the clicks showed is
// that the cost that mattered was sinf, not the loop around it, so play_tone
// keeps the loop and drops the sinf. One period of a sine lives in the 256-entry
// wave[], and a 32-bit phase accumulator walks it: the top 8 bits index, the
// next 8 weight a linear interpolation between neighbours, and the step is the
// frequency scaled by 2^32/24000, so any frequency is exact to a fraction of a
// hertz without a division per sample. That leaves a handful of integer
// multiplies per sample against sinf's several hundred cycles -- an operation
// count, not a measurement.
//
// The synthesis these tables replace is kept verbatim in main/hal/sfx_synth.h,
// where tools/test_sfx.py runs it and diffs it against what the generator
// emitted. Worst-case difference over all 3,240 click samples and all 256 wave
// samples: 0.

// Regenerates both tables with this chip's own libm and reports how far the
// baked ones are from what it would have produced.
//
// tools/test_sfx.py already diffs the generator against sfx_synth.h on a host,
// and finds zero -- but that compares Python's libm to the host's glibc, and
// the only sinf that ever mattered is the xtensa newlib one that ran here. A
// difference of 1 is -90 dBFS and inaudible; the reason to measure it anyway is
// that "probably inaudible" is the kind of sentence this project has been wrong
// about before. Runs from the ui task on request, never at boot, so the 3,240
// samples of scratch and the ~77 ms of sinf cost nothing the rest of the time.
void sound_check_tables(void) {
    int16_t *scratch=malloc(SFX_SAMPLES*sizeof(int16_t));
    if(!scratch){ESP_LOGW("sound","TABLES no room for %d bytes",
                          (int)(SFX_SAMPLES*sizeof(int16_t)));return;}
    int peak=0,at=0,where=0;   // where: click index, or -1 for the wave
    for(int k=0;k<SFX_KINDS;k++) {
        sfx_ref_synthesize(k,scratch+sfx_offset[k]);
        for(int n=0;n<sfx_frames[k];n++) {
            int d=sfx_pcm[sfx_offset[k]+n]-scratch[sfx_offset[k]+n];
            if(d<0)d=-d;
            if(d>peak){peak=d;at=n;where=k;}
        }
    }
    // The wave is 256 of the 3,240 the scratch already holds.
    sfx_ref_wave(scratch);
    for(int i=0;i<WAVE_POINTS;i++) {
        int d=wave[i]-scratch[i];
        if(d<0)d=-d;
        if(d>peak){peak=d;at=i;where=-1;}
    }
    free(scratch);
    ESP_LOGI("sound","TABLES worst=%d of 32767 where=%d at=%d",peak,where,at);
}

// Sends one 128-frame block and says whether the channel is still healthy.
static bool emit(const int16_t *pcm) {
    size_t written=0;
    esp_err_t err=i2s_channel_write(output,pcm,256*sizeof(int16_t),&written,100);
    if(err==ESP_OK&&written==256*sizeof(int16_t))return true;
    ESP_LOGW("sound","I2S write failed");
    return false;
}

static void play_click(int kind,int16_t *pcm) {
    int frames=sfx_frames[kind];
    // A tail of silence past the end pushes the last samples through the
    // DMA ring, as the synthesised version's frames+256 did.
    for(int start=0;start<frames+256;start+=128) {
        for(int j=0;j<128;j++) {
            int n=start+j;
            int16_t sample=(n<frames && atomic_load(&enabled))?sfx_pcm[sfx_offset[kind]+n]:0;
            pcm[j*2]=pcm[j*2+1]=sample;
        }
        if(!emit(pcm))return;
    }
    // The only evidence from outside that a click reached the codec, and what
    // tools/test_settings.py asserts on: the queue accepting a request proves
    // nothing about the I2S write that follows. It said "synthesized" until the
    // clicks stopped being synthesized per play; the check it backs is the same.
    ESP_LOGI("sound","SFX %d played %d frames",kind,frames);
}

static void play_tone(const request_t *req,int16_t *pcm) {
    uint32_t phase=0,step=(uint32_t)(((uint64_t)req->frequency<<32)/SAMPLE_RATE);
    uint32_t frames=req->frames;
    // The same 3 ms attack the clicks use, and a release to match, so a tone
    // neither starts nor stops on a step in the waveform. Short tones get half
    // their length at each end instead.
    uint32_t ramp=frames/2<72?frames/2:72;
    bool completed=true;
    for(uint32_t start=0;start<frames+256;start+=128) {
        if(atomic_load(&cancelled)==req->id){completed=false;break;}
        for(int j=0;j<128;j++) {
            uint32_t n=start+j;
            int value=0;
            if(n<frames&&atomic_load(&enabled)) {
                unsigned index=phase>>24,frac=(phase>>16)&0xff;
                int s=(wave[index]*(int)(256-frac)+wave[(index+1)&(WAVE_POINTS-1)]*(int)frac)>>8;
                int envelope=4096;
                if(ramp) {
                    if(n<ramp)envelope=(int)(n*4096/ramp);
                    else if(n>=frames-ramp)envelope=(int)((frames-n)*4096/ramp);
                }
                value=((s*envelope)>>12)*req->gain>>12;
            }
            phase+=step;
            pcm[j*2]=pcm[j*2+1]=(int16_t)value;
        }
        if(!emit(pcm)){completed=false;break;}
    }
    if(req->done)req->done(req->ctx,completed);
}

// ------------------------------------------------------------------- clips
//
// The whole clip is already in the caller's RAM and is read in place, so this
// adds no ring buffer, no second task and no second I2S channel -- it is the
// same 128-frame block the clicks and tones write, filled from a decoder
// instead of from a table. That is the only shape of playback this board has
// room for; docs/common-api.md 9.1 carries the measurements that rule out the
// alternative.
//
// The IMA ADPCM decoder is in ima_adpcm.h, where tools/test_ima.py can compile
// the same lines this task runs. It costs 194 bytes of flash for its two tables
// and 20 bytes of state on this task's stack; that is the whole of the codec.

// The clip the audio task is inside, and the id whoever wants it stopped last
// asked for. Both are the whole of the handshake in sound_clip_stop().
static atomic_int clip_active;
static atomic_int clip_halt;
static atomic_uint clip_frames;   // output frames produced so far

static void play_clip(const request_t *req,int16_t *pcm) {
    // Claim first, then look for a stop: sound_clip_stop() writes the halt and
    // then reads this, so with sequentially consistent atomics one of the two
    // sides always sees the other. Either this returns without touching the
    // caller's bytes, or the stopper waits for it to finish. There is no
    // interleaving where the buffer is freed under a read.
    atomic_store(&clip_active,req->id);
    atomic_store(&clip_frames,0);
    if(atomic_load(&clip_halt)==req->id) {
        atomic_store(&clip_active,0);
        if(req->done) req->done(req->ctx,false);
        return;
    }
    ima_t ima={.data=req->data,.bytes=req->bytes,.block=req->block};
    uint32_t frames=req->frames, at=0;
    bool completed=true;
    // A tail of silence past the end, as the clicks have, to push the last
    // samples through the DMA ring.
    while(at<frames+256) {
        if(atomic_load(&clip_halt)==req->id) { completed=false; break; }
        for(int j=0;j<128;j++,at++) {
            int sample=0;
            if(at<frames) {
                if(req->block) sample=ima_next(&ima);
                else {
                    uint32_t off=at*2u;
                    sample=off+1<req->bytes
                        ?(int16_t)(req->data[off]|(req->data[off+1]<<8)):0;
                }
                // Muting silences a clip without shortening it, the same way it
                // treats a tone: what is heard changes, not how long it lasts.
                if(!atomic_load(&enabled)) sample=0;
                sample=(sample*req->gain)>>12;
            }
            pcm[j*2]=pcm[j*2+1]=(int16_t)sample;
        }
        atomic_store(&clip_frames,at<frames?at:frames);
        if(!emit(pcm)) { completed=false; break; }
    }
    atomic_store(&clip_active,0);
    if(req->done) req->done(req->ctx,completed);
}

int32_t sound_clip_start(const uint8_t *data,uint32_t bytes,int format,
                         uint16_t block,uint32_t frames,float gain,
                         sound_done_fn done,void *ctx) {
    if(!events) return SOUND_ERR_UNSUPPORTED;
    if(!data||!bytes||!frames) return SOUND_ERR_INVALID;
    if(!(gain>=0.0f&&gain<=1.0f)) return SOUND_ERR_INVALID;
    // A block has a four-byte header and at least one nibble pair after it, and
    // has to be even for the nibble walk above to end where the next block
    // begins. PCM16 has no blocks at all.
    if(format==SOUND_CLIP_IMA) { if(block<8||block&1) return SOUND_ERR_INVALID; }
    else if(format==SOUND_CLIP_PCM16) block=0;
    else return SOUND_ERR_INVALID;
    request_t req={
        .id=atomic_fetch_add(&next_id,1),
        .kind=-2,
        .gain=(uint16_t)(gain*4096.0f),
        .frames=frames,
        .done=done,.ctx=ctx,
        .data=data,.bytes=bytes,.block=block};
    if(xQueueSend(events,&req,0)!=pdTRUE) return SOUND_ERR_BUSY;
    return req.id;
}

bool sound_clip_stop(int32_t id) {
    if(id<=0) return true;
    atomic_store(&clip_halt,id);
    // One block is 5.3ms and the write it may be inside gives up after 100ms,
    // so 200ms is well past any honest wait. Returning false would mean the
    // audio task still holds the caller's bytes, which is not a thing to
    // recover from by freeing them anyway.
    for(int i=0;i<40&&atomic_load(&clip_active)==id;i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    return atomic_load(&clip_active)!=id;
}

uint32_t sound_clip_position(int32_t id) {
    return atomic_load(&clip_active)==id?atomic_load(&clip_frames):0;
}

int32_t sound_tone(unsigned frequency_hz,unsigned duration_ms,float gain,
                   sound_done_fn done,void *ctx) {
    if(!events)return SOUND_ERR_UNSUPPORTED;
    if(frequency_hz<SOUND_TONE_MIN_HZ||frequency_hz>SOUND_TONE_MAX_HZ)return SOUND_ERR_INVALID;
    if(!duration_ms||duration_ms>SOUND_TONE_MAX_MS)return SOUND_ERR_INVALID;
    // Written as a positive test so that a NaN gain is rejected rather than
    // slipping past two comparisons that are both false.
    if(!(gain>=0.0f&&gain<=1.0f))return SOUND_ERR_INVALID;
    request_t req={
        .id=atomic_fetch_add(&next_id,1),
        .kind=-1,
        .frequency=(uint16_t)frequency_hz,
        .gain=(uint16_t)(gain*4096.0f),
        .frames=duration_ms*(SAMPLE_RATE/1000),
        .done=done,.ctx=ctx};
    if(xQueueSend(events,&req,0)!=pdTRUE)return SOUND_ERR_BUSY;
    return req.id;
}

static void audio_task(void *arg) {
    (void)arg;request_t req;int16_t pcm[256];
    while(1) {
        xQueueReceive(events,&req,portMAX_DELAY);
        if(req.kind>=0) {
            if(req.kind<SFX_KINDS&&atomic_load(&enabled))play_click(req.kind,pcm);
        } else if(req.kind==-2) {
            play_clip(&req,pcm);
        } else {
            play_tone(&req,pcm);
        }
    }
}
void sound_init(i2c_master_bus_handle_t bus) {
    if(i2c_master_probe(bus,0x18,30)!=ESP_OK){ESP_LOGW("sound","ES8311 unavailable");return;}
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);
    channel.dma_desc_num=4;channel.dma_frame_num=128;channel.auto_clear=true;
    esp_err_t err=i2s_new_channel(&channel,&output,NULL);
    if(err!=ESP_OK){ESP_LOGW("sound","I2S channel unavailable: %s",esp_err_to_name(err));return;}
    i2s_std_config_t cfg={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
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
    events=xQueueCreate(4,sizeof(request_t));
    if(!events)goto fail;
    if(xTaskCreate(audio_task,"sfx",4096,NULL,7,NULL)!=pdPASS){vQueueDelete(events);events=NULL;goto fail;}
    // Nothing is rendered here any more; the tables were rendered by the build.
    ESP_LOGI("sound","ES8311 ready; 24kHz stereo from %d baked samples; default ON",
             SFX_SAMPLES+WAVE_POINTS);return;
fail:
    ESP_LOGW("sound","Audio unavailable: %s",esp_err_to_name(err));
    i2s_channel_disable(output);i2s_del_channel(output);output=NULL;
}
