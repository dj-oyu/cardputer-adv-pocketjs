#include "sound.h"
#include "ima_adpcm.h"
#include "driver/i2s_std.h"
#include "esp_cpu.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
// sfx_pcm / sfx_offset / sfx_frames / wave, generated into the build tree by
// tools/make_sfx.py. The block above play_click says why they are not in .bss.
#include "sfx_tables.h"
// The synthesis those tables replace, kept so the board can check them.
#include "sfx_synth.h"
#include <stdatomic.h>
#include <stdlib.h>

#define SAMPLE_RATE ((int)SOUND_SAMPLE_RATE)

// A click, a tone, or a stream. kind is the click index, -1 for a tone, -2 for
// a stream; frequency is read only for tones, and the last two fields only for
// streams. One struct rather than a union because the queue is four entries
// deep: the eight bytes a stream adds cost 32 bytes of .bss in total, and a
// union would cost the same reading twice as badly.
typedef struct {
    int32_t id;
    int16_t kind;
    uint16_t frequency;
    uint16_t gain;        // 0..4096
    uint32_t frames;
    sound_done_fn done;
    void *ctx;
    sound_stream_t *stream;         // streams: the caller's ring
    uint16_t block;       // streams: ADPCM block size, 0 for PCM16
} request_t;

static i2s_chan_handle_t output;
static QueueHandle_t events;
static atomic_bool enabled=true;
static atomic_int cancelled;
static atomic_int next_id=1;
// True from sound_capture_start() to sound_capture_stop(). Playback reads it,
// which is where "recording and app playback are exclusive, and UI cues do not
// sound while recording" (docs/common-api.md 9) is actually enforced.
static atomic_bool capturing;
// True while the audio task is inside a request. Together with the queue's
// depth it is what sound_capture_start() asks before it takes the codec.
static atomic_bool playing;
void sound_set_enabled(bool value){atomic_store(&enabled,value);}
bool sound_available(void){return events!=NULL;}
bool sound_play(int kind) {
    if(!events||!atomic_load(&enabled)||atomic_load(&capturing))return false;
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

// ----------------------------------------------------------------- streams
//
// The consumer half of sound.h's ring. The producer -- pocket_av.c's player,
// running on the JS/ui task -- reads the source and publishes slots; this task
// takes them in order, decodes, and writes the same 128-frame block the clicks
// and tones write. No second I2S channel and no second task: the ring is the
// only thing this adds, and the caller owns it.
//
// What replaced what: the previous shape took the whole sound in one buffer and
// read it in place, which made the buffer the ceiling on length. Nothing about
// the audio task changes here except where the bytes come from.
//
// The IMA ADPCM decoder is in ima_adpcm.h, where tools/test_ima.py can compile
// the same lines this task runs. It costs 194 bytes of flash for its two tables
// and 20 bytes of state on this task's stack; that is the whole of the codec.

// The stream the audio task is inside, and the id whoever wants it stopped last
// asked for. Both are the whole of the handshake in sound_stream_stop().
static atomic_int stream_active;
static atomic_int stream_halt;
static atomic_uint stream_frames;      // output frames produced so far
static atomic_uint stream_starved;     // blocks filled with silence

// The underrun policy is stated in full above sound_stream_start() in sound.h,
// because it is the part of this design an app can see. The two lines it comes
// to here: a starved block is silence and the source does not advance, and a
// producer that has published nothing for SOUND_STREAM_STARVE_BLOCKS blocks in
// a row ends the stream as incomplete.

static void play_stream(const request_t *req,int16_t *pcm) {
    sound_stream_t *s=req->stream;
    // Claim first, then look for a stop: sound_stream_stop() writes the halt and
    // then reads this, so with sequentially consistent atomics one of the two
    // sides always sees the other. Either this returns without touching the
    // caller's ring, or the stopper waits for it to finish. There is no
    // interleaving where the ring is freed under a read.
    atomic_store(&stream_active,req->id);
    atomic_store(&stream_frames,0);
    atomic_store(&stream_starved,0);
    if(atomic_load(&stream_halt)==req->id) {
        atomic_store(&stream_active,0);
        if(req->done) req->done(req->ctx,false);
        return;
    }
    stream_read_t r={0};
    uint32_t frames=req->frames, at=0, starved=0;
    bool completed=true;
    // A tail of silence past the end, as the clicks have, to push the last
    // samples through the DMA ring.
    while(at<frames+256) {
        if(atomic_load(&stream_halt)==req->id) { completed=false; break; }
        // One 128-frame block; stream_sample() in sound_stream.h is the walk.
        bool starving=false;
        for(int j=0;j<128;j++) {
            int sample=0;
            if(at<frames) {
                bool ended=false;
                if(stream_sample(s,&r,req->block,&sample,&ended)) {
                    at++;
                    // Muting silences a stream without shortening it, the same
                    // way it treats a tone: what is heard changes, not how long
                    // it lasts.
                    if(!atomic_load(&enabled)) sample=0;
                    sample=(sample*req->gain)>>12;
                } else if(ended) {
                    // The source ended shorter than its header claimed. What is
                    // left of the loop is the silence tail.
                    frames=at; at++; sample=0;
                } else {
                    // The producer is behind. Silence for this sample and the
                    // same position next time, so an underrun stretches the
                    // sound rather than dropping part of it -- and
                    // status().positionMs stalls with it, which is the truth.
                    starving=true; sample=0;
                }
            } else at++;
            pcm[j*2]=pcm[j*2+1]=(int16_t)sample;
        }
        if(starving) {
            starved++;
            atomic_store(&stream_starved,atomic_load(&stream_starved)+1);
            if(starved>=SOUND_STREAM_STARVE_BLOCKS) { completed=false; break; }
        } else starved=0;
        atomic_store(&stream_frames,at<frames?at:frames);
        if(!emit(pcm)) { completed=false; break; }
    }
    stream_release(s,&r);
    atomic_store(&stream_active,0);
    if(req->done) req->done(req->ctx,completed);
}

int32_t sound_stream_start(sound_stream_t *stream,int format,uint16_t block,
                           uint32_t frames,float gain,
                           sound_done_fn done,void *ctx) {
    if(!events) return SOUND_ERR_UNSUPPORTED;
    // The codec is recording. BUSY rather than UNSUPPORTED: the feature exists
    // and the answer changes when the recording closes.
    if(atomic_load(&capturing)) return SOUND_ERR_BUSY;
    if(!stream||!stream->bytes||!frames) return SOUND_ERR_INVALID;
    if(!(gain>=0.0f&&gain<=1.0f)) return SOUND_ERR_INVALID;
    // A block has a four-byte header and at least one nibble pair after it, and
    // has to be even for the nibble walk to end where the next block begins. It
    // also has to fit a slot whole: a slot ending mid-block would leave the
    // decoder with nothing to reseed from. PCM16 has no blocks at all.
    if(format==SOUND_STREAM_IMA) {
        if(block<8||block&1||block>SOUND_STREAM_SLOT_BYTES) return SOUND_ERR_INVALID;
    } else if(format==SOUND_STREAM_PCM16) block=0;
    else return SOUND_ERR_INVALID;
    request_t req={
        .id=atomic_fetch_add(&next_id,1),
        .kind=-2,
        .gain=(uint16_t)(gain*4096.0f),
        .frames=frames,
        .done=done,.ctx=ctx,
        .stream=stream,.block=block};
    if(xQueueSend(events,&req,0)!=pdTRUE) return SOUND_ERR_BUSY;
    return req.id;
}

bool sound_stream_stop(int32_t id) {
    if(id<=0) return true;
    atomic_store(&stream_halt,id);
    // One block is 5.3ms and the write it may be inside gives up after 100ms,
    // so 200ms is well past any honest wait. Returning false would mean the
    // audio task still holds the caller's ring, which is not a thing to
    // recover from by freeing it anyway.
    for(int i=0;i<40&&atomic_load(&stream_active)==id;i++)
        vTaskDelay(pdMS_TO_TICKS(5));
    return atomic_load(&stream_active)!=id;
}

uint32_t sound_stream_position(int32_t id) {
    return atomic_load(&stream_active)==id?atomic_load(&stream_frames):0;
}

uint32_t sound_stream_underruns(void) { return atomic_load(&stream_starved); }

int32_t sound_tone(unsigned frequency_hz,unsigned duration_ms,float gain,
                   sound_done_fn done,void *ctx) {
    if(!events)return SOUND_ERR_UNSUPPORTED;
    if(atomic_load(&capturing))return SOUND_ERR_BUSY;
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

// ----------------------------------------------------------------- capture
//
// The RX half of the same controller. i2s_acquire_controller_obj() hands back
// the controller this file already owns (i2s_common.c), i2s_take_available_
// channel() finds the RX direction free, and the STD init below matches the
// TX configuration field for field -- which is what makes IDF treat the pair as
// full duplex and share BCLK/WS instead of trying to drive pins TX reserved.
// Match it or the channel is not a microphone, it is a bus conflict.
//
// Everything here exists only between start and stop. The channel, its DMA
// buffers and the codec's ADC power are all taken at start and given back at
// stop, so an app that never records pays nothing, which on a board with no
// PSRAM is the difference that decides whether this feature may exist at all.

// 6 descriptors of 256 frames. The slot configuration is TX's, so a frame is
// stereo 16-bit -- 4 bytes -- and the ring is 6,144 bytes for 64 ms of audio.
//
// Half of those bytes are thrown away. The microphone is one channel, but the
// slots have to match TX field for field or the pair is not full duplex, so the
// DMA is fed a stereo frame and sound_capture_read() keeps the first slot and
// drops the second. That is why 6,144 bytes buys 64 ms and not 128, and it is
// the reason not to halve this number: halving it halves the time, not the
// waste. The waste is the price of one controller serving both directions and
// cannot be paid off here.
//
// 64 ms is about two UI frames at the 33 ms this firmware draws at, so a
// program reading once a frame can miss one frame entirely and still lose
// nothing. Miss two and the driver reports an overflow, which is a report and
// not a splice -- see sound_capture_read.

#define CAPTURE_DESC_NUM   6
#define CAPTURE_FRAME_NUM  256

static i2c_master_bus_handle_t codec_bus;
static i2s_chan_handle_t input;
static atomic_bool overflowed;
// Frames taken out of the DMA ring since sound_capture_start(), by whichever
// reader is doing it. The diagnostic reads through sound_capture_read() too
// now, so this is one counter with one meaning, because "CAPTURE STOP frames=" is one
// marker and it cannot mean two things: the probe read 28,800 frames and the
// line said 0, which is a marker lying about a number it exists to report.
// The level of what was last read, for the recording indicator. Written by
// whichever task is reading and read by the drawing task, so both halves are
// atomic and neither blocks: a stale value costs one frame of a moving bar.
//
// `level_us` is when it was written, and the getter fades it out from there
// rather than the writer decaying it. That matters because the writer only runs
// when the app reads: an app that stops reading would otherwise freeze the bar
// at whatever it last saw, which is a display asserting something it does not
// know any more.
static atomic_uint level_peak;    // 0..32767
static atomic_int  level_us;      // when level_peak was written
static atomic_int  clip_us;       // when a sample last hit the rail

static atomic_uint captured;

// One register block to the codec at 0x18. The device handle is added and
// removed around the writes, as sound_init does, so nothing holds a bus device
// between recordings.
static esp_err_t codec_write(const uint8_t (*regs)[2], unsigned count) {
    if(!codec_bus) return ESP_ERR_INVALID_STATE;
    i2c_master_dev_handle_t codec;
    i2c_device_config_t dev={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
                             .device_address=0x18,.scl_speed_hz=100000};
    esp_err_t err=i2c_master_bus_add_device(codec_bus,&dev,&codec);
    if(err!=ESP_OK) return err;
    for(unsigned i=0;i<count;i++) {
        err=i2c_master_transmit(codec,regs[i],2,30);
        if(err!=ESP_OK) break;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    i2c_master_bus_rm_device(codec);
    return err;
}

// Reads registers back off the codec. Nothing in this file did that until a
// diagnostic row reported a dead input as a hot signal, and the first question
// -- does the chip actually hold the value we wrote? -- turned out to be
// unanswerable. A write that is ACKed is not a register that stuck: the ACK is
// the chip saying it heard an address, not that the value survived a reset, a
// power-down block or a later write. Used only by the probe.
static esp_err_t codec_read(uint8_t reg, uint8_t *out) {
    if(!codec_bus) return ESP_ERR_INVALID_STATE;
    i2c_master_dev_handle_t codec;
    i2c_device_config_t dev={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
                             .device_address=0x18,.scl_speed_hz=100000};
    esp_err_t err=i2c_master_bus_add_device(codec_bus,&dev,&codec);
    if(err!=ESP_OK) return err;
    err=i2c_master_transmit_receive(codec,&reg,1,out,1,30);
    i2c_master_bus_rm_device(codec);
    return err;
}

// The codec's input path.
//
// UNVERIFIED ON THIS BOARD, and no host can settle it: ESP-IDF v6.0.1 ships no
// ES8311 driver, nothing is vendored here, and board_capture() sees the frame
// buffer rather than what the codec does. These come from the ES8311 register
// map and from the shape of the playback block above it, which was itself taken
// from M5Unified's verified Cardputer ADV callback. The playback registers are
// not touched, so a wrong value here can only make a recording silent or noisy;
// it cannot break the speaker.
//
// 0x01 is the exception worth naming: it is the clock manager, shared by both
// directions, and 0xb5 (playback's value) leaves the ADC clocks off. 0xbf turns
// them on and is restored to 0xb5 at stop, so a bad guess here is confined to
// the length of a recording.
// 0x14 says where the ADC's input comes from. 0x1a is an analog microphone on
// the codec's own pins; 0x5a is the same with bit 6 set, which takes a PDM
// microphone in on the DMIC pins instead.
//
// This board is ANALOG. That was established late and expensively, and the
// opposite was believed all day, so the evidence is here in full. Profiled on
// the hardware in 150 ms buckets while somebody spoke (2026-09-08):
//
//   analog  2033  885  309  228  342  1312  587  1152  928  865   <- speech
//   pdm     3660 3660   25    0    0     0    0     0    0    0   <- not speech
//
// The analog row varies bucket to bucket, which is what a voice does. The PDM
// row is the same number to the digit for 300 ms and then decays to nothing:
// that is a constant, not a sound. Disabling the ADC's DC-offset cancellation
// (0x1c) pinned every PDM bucket at 32,767, which is what a large fixed level
// looks like with the filter that was hiding it removed.
//
// So the PDM pins carry a DC offset and no microphone, the high-pass takes
// about 450 ms to converge on it, and every PDM measurement taken before this
// -- the 9-versus-227 comparison that chose this register in the first place,
// every gain sweep, and every recording apps/miccheck made -- was reading that
// offset on its way out. miccheck's recordings are 0.64 s, which fits almost
// entirely inside the 450 ms window, so its peaks looked like speech by
// coincidence of timing.
//
// A variable so sound_capture_probe() can sweep it.
static uint8_t mic_reg14=0x1a;

// The ES8311's ADC automute and ALC are NOT involved in anything here, and this
// is written down so the next person does not spend a run finding out: 0x18 bit
// 6 is automute and bit 7 is ALC, the datasheet gives both a reset default of
// 0, and nothing in this file writes 0x18 at all. It was also tested rather
// than trusted -- a probe row that wrote 0x18, 0x19 and 0x1a explicitly to zero
// behaved exactly like the control row. Automute is the first thing a reader
// suspects when audio stops partway through a recording, and on this board it
// has been eliminated twice.

// 0x16 is the PGA step, 0..7 for 0 to 42 dB, and on this board it is the gain
// that matters: the microphone is on the analog input, so this sits in front of
// the converter and buys real signal-to-noise where 0x17 behind it does not.
//
// 0x07, the maximum, measured rather than assumed: dropping to 0x06 and 0x05 at
// the same digital gain lost level and gained nothing, and nothing clipped at
// any setting. See mic_reg17 for the table.
static uint8_t mic_reg16=0x07;
// The ADC's digital volume, applied after conversion, and the second of the two
// gains on the analog path.
//
// 0xc8, and what follows is what was MEASURED AND UNDER WHAT CONDITIONS, not a
// property of the board. The difference matters here more than usual: a level
// is a fact about a microphone, a voice, and a distance, and every number below
// was taken with one person speaking at one distance.
//
// Sweep, five rows, one speaker at one distance (2026-09-08). Comparable with
// each other, and that is all they are good for:
//
//   0x16  0x17    rms          peak            clip
//    7    a0      114..1766     383.. 8554      0
//    7    b8      248..1209    1016.. 3762      0
//    7    c8     1059..2340    3522.. 8395      0     <- chosen
//    6    c8      931..1306    3381..10694      0
//    5    c8      431.. 737    1410.. 3809      0
//
// The rule that chose the row is the most PGA that does not clip, with the
// digital volume making up the rest: PGA is in front of the converter and buys
// signal-to-noise, digital gain is behind it and buys only use of the range.
// The rows support that -- one PGA step moves rms about 25% at fixed digital
// gain, the digital gain moves it much further.
//
// The same registers, minutes later, with the same person closer and louder
// (apps/miccheck, three recordings): peaks of 22,539, 30,528 and 31,779, which
// is 69%, 93% and 97% of full scale. So the sweep's "8,395, a quarter of full
// scale, there is headroom" was a fact about that afternoon's speaking
// distance, worth a factor of four, and NOT a property of this gain. 31,779 of
// 32,767 is a syllable from the rail.
//
// Three recordings at ordinary speaking level, later still: peaks of 17,816,
// 22,559 and 27,825 -- 54%, 69% and 85% -- with clip=0 on all three.
//
// So the honest sentence about this setting is not "it does not clip". It is:
// IT DOES NOT CLIP AT ORDINARY SPEAKING LEVEL, AND IT DOES CLIP WHEN SOMEBODY
// SPEAKS UP. 85% is about 1.4 dB from the rail and a raised voice went past it
// twenty minutes earlier. That is a property of the setting, not a failure of
// it, and the meter's amber cell is what tells the person making the sound.
//
// Which way to err, if this has to move: DOWN. This gain is applied after
// conversion, so lowering it costs use of the int16 range and no
// signal-to-noise at all, while raising it risks clipping, which destroys audio
// outright. The asymmetry is the argument, not a target number.
//
// For a loud room, or a program that would rather lose range than a syllable:
// drop 0x17 by eight steps at a time and re-run apps/miccheck, which prints
// clip= per recording. Do not derive the new value from a decibel arithmetic --
// the step size of this register has never been verified on this board, and the
// one time it was claimed it came from rows that were measuring a DC offset.
static uint8_t mic_reg17=0xc8;

// Writes the input path with the current 0x14/0x16.
//
// Every other value here comes from **Espressif's own es8311 component**
// (esp-bsp/components/es8311, reachable through IDF's i2s_es8311 example), not
// from the datasheet -- and which source it came from is the thing that made
// the difference. The datasheet-derived table this file carried first was
// wrong in three places at once: 0x17 was 0xbf where the driver writes 0xc8,
// 0x16 was never written at all, and 0x15/0x1b were written although the driver
// touches neither. 0x01 is the driver's 0x3f "all clocks on" with bit 7 for
// MCLK-from-SCLK, which is exactly the half of the clock manager that
// playback's 0xb5 leaves off; 0x0e 0x02 powers the PGA and the ADC modulator.
// Extra register writes appended to the input block, for sound_capture_probe()
// to try one configuration against another. NULL for a recording.
static const uint8_t (*extra_regs)[2];
static unsigned extra_count;

static esp_err_t capture_codec_on(void) {
    const uint8_t regs[][2]={
        {0x01,0xbf},   // clock manager: playback's 0xb5 plus the ADC clocks
        {0x0e,0x02},   // analog power: PGA and ADC modulator up
        {0x14,mic_reg14},
        {0x16,mic_reg16},
        {0x17,mic_reg17},   // ADC digital volume; see mic_reg17
        {0x1c,0x6a},   // ADC equalizer bypass, DC offset cancellation
        {0x44,0x00},   // no ADC-to-DAC loopback
        {0x0a,0x00},   // SDP out: 16-bit I2S, the slot the RX channel reads
    };
    esp_err_t err=codec_write(regs,sizeof regs/sizeof regs[0]);
    if(err==ESP_OK && extra_regs && extra_count)
        err=codec_write(extra_regs,extra_count);
    return err;
}
static const uint8_t CAPTURE_OFF[][2]={
    {0x0e,0xff},   // analog power: ADC down
    {0x14,0x00},   // analog MIC off
    {0x01,0xb5},   // clock manager back to what playback was initialised with
};

// The driver's own report that the DMA wrapped onto audio nobody had read. Runs
// in the I2S interrupt, so it does one relaxed store and nothing else.
//
// What an overflow does and does not do, from i2s_common.c's
// i2s_dma_rx_callback: when the message queue is full the driver drops the
// OLDEST descriptor, fires this, and posts the new one. The channel is never
// stopped and i2s_channel_read() keeps returning data. So an overrun costs old
// audio and never future audio -- which means a stalled or silent stream is
// never explained by an overflow, and a whole family of "the ring overran and
// everything after it died" theories can be ruled out without a run. Worth
// knowing before reading the driver again: it took an afternoon to establish
// once.
static bool capture_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event,
                             void *user) {
    (void)handle; (void)event; (void)user;
    atomic_store(&overflowed,true);
    return false;   // no task woken
}

bool sound_capture_active(void){return atomic_load(&capturing);}
bool sound_capture_overflowed(void){return atomic_exchange(&overflowed,false);}

void sound_capture_level(unsigned *peak_out, bool *clipping_out) {
    int64_t now=esp_timer_get_time();
    int written=atomic_load(&level_us);
    unsigned peak=atomic_load(&level_peak);
    // Held for 150ms, then gone. Long enough that a bar tracks syllables rather
    // than flickering per read, short enough that a program which stopped
    // reading stops claiming a level within a frame or two.
    int64_t age=now-(int64_t)written;
    if(!written || age>150000) peak=0;
    else {
        // Decay the peak across the hold so the bar falls rather than steps.
        peak=(unsigned)((uint64_t)peak*(unsigned)(150000-age)/150000u);
        atomic_store(&level_peak,peak);
    }
    if(peak_out) *peak_out=peak;
    // Clipping is latched for a second: a single clipped sample is a real
    // event and a bar that showed it for 20ms would not be seen by a person.
    if(clipping_out) {
        int at=atomic_load(&clip_us);
        *clipping_out = at && now-(int64_t)at < 1000000;
    }
}
uint32_t sound_capture_frames(void){return atomic_load(&captured);}

bool sound_capture_start(void) {
    // What this refuses: a board with no codec, and no I2C to configure it.
    //
    // What it deliberately does NOT test is whether a recording is already
    // running. The compare-exchange below is the single authority on that, and
    // it is atomic where a second check would not be. `input` used to appear
    // here and meant "already recording" -- true only while the channel's
    // lifetime was the recording's. When the channel became persistent that
    // term silently changed meaning to "has ever recorded", so the first
    // recording of a session was also the last: every later start returned
    // false before doing anything. A condition that was true of the old
    // arrangement, left in place across the change that made it mean something
    // else.
    if(!events||!codec_bus) return false;
    // Claimed before the queue is looked at, not after: a click accepted
    // between the two checks would otherwise play into the recording. With the
    // flag up first, sound_play() and sound_tone() are already refusing, so
    // what is left to wait for is only what was accepted before this call.
    bool expected=false;
    if(!atomic_compare_exchange_strong(&capturing,&expected,true)) return false;
    if(atomic_load(&playing)||uxQueueMessagesWaiting(events)) {
        atomic_store(&capturing,false);
        return false;
    }
    atomic_store(&overflowed,false);
    atomic_store(&captured,0);
    atomic_store(&level_peak,0);
    atomic_store(&level_us,0);
    atomic_store(&clip_us,0);

    // Created here and destroyed in sound_capture_stop(), so the DMA -- 6
    // descriptors of 256 stereo frames, 6,144 bytes -- exists only while a
    // recording does, and an app that never records pays nothing.
    //
    // It was kept alive for an afternoon on the hypothesis that
    // i2s_del_channel() does not undo full-duplex constitution and that the
    // second recording of a session was therefore dead. The mechanism was read
    // out of i2s_std.c, it was specific, it was plausible, and it was never
    // true of the product: that failure existed only inside a diagnostic which
    // could not read audio at all, and whose control row was silent too. An app
    // doing open/close/open three times over records the same level every time
    // (apps/miccheck: peaks 14,433 / 15,414 / 14,696, one session). So the
    // bytes went back to being temporary, and the lesson stayed: a mechanism
    // read out of driver source is a hypothesis until the product path has been
    // asked.
    esp_err_t err=ESP_OK;
    {
        i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1,I2S_ROLE_MASTER);
        channel.dma_desc_num=CAPTURE_DESC_NUM;
        channel.dma_frame_num=CAPTURE_FRAME_NUM;
        err=i2s_new_channel(&channel,NULL,&input);
        if(err!=ESP_OK) { input=NULL; goto fail; }
        // Field for field what sound_init() gave the TX channel, din=46
        // (ASDOUT) in place of dout. Differ anywhere and the pair is not full
        // duplex, and this is the one moment the sharing is established.
        i2s_std_config_t cfg={.clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
            .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO),
            .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=41,.ws=43,.dout=I2S_GPIO_UNUSED,.din=46}};
        err=i2s_channel_init_std_mode(input,&cfg);
        if(err!=ESP_OK) goto fail;
        i2s_event_callbacks_t callbacks={.on_recv_q_ovf=capture_overflow};
        err=i2s_channel_register_event_callback(input,&callbacks,NULL);
        if(err!=ESP_OK) goto fail;
    }
    err=capture_codec_on();
    if(err!=ESP_OK) goto fail;
    err=i2s_channel_enable(input);
    if(err!=ESP_OK) goto fail;
    // Byte-stable from here: tools/ treat these three lines as a contract.
    ESP_LOGI("sound","CAPTURE START rate=%u dma=%ux%u",
             (unsigned)SAMPLE_RATE,(unsigned)CAPTURE_DESC_NUM,
             (unsigned)CAPTURE_FRAME_NUM);
    return true;
fail:
    ESP_LOGW("sound","CAPTURE START failed: %s",esp_err_to_name(err));
    if(input) { i2s_del_channel(input); input=NULL; }
    codec_write(CAPTURE_OFF,sizeof CAPTURE_OFF/sizeof CAPTURE_OFF[0]);
    atomic_store(&capturing,false);
    return false;
}

int sound_capture_read(int16_t *out, int max_frames) {
    if(!input||!atomic_load(&capturing)||!out) return -2;
    if(max_frames<=0) return 0;
    if(max_frames>SOUND_CAPTURE_MAX_FRAMES) max_frames=SOUND_CAPTURE_MAX_FRAMES;
    // Read before write: an overflow already reported means the audio that
    // follows it is not continuous with the audio before it, so nothing more
    // may be handed out under the promise that it is.
    if(atomic_load(&overflowed)) return -1;
    // 128 stereo frames on the caller's stack. The DMA holds stereo because the
    // slots are TX's, and the second slot is dropped here rather than in the
    // guest; 512 bytes is the whole of the conversion's working set.
    int16_t scratch[256];
    int got=0;
    while(got<max_frames) {
        size_t want=(size_t)(max_frames-got)*2u*sizeof(int16_t);
        if(want>sizeof scratch) want=sizeof scratch;
        size_t read=0;
        esp_err_t err=i2s_channel_read(input,scratch,want,&read,0);
        if(err==ESP_ERR_TIMEOUT||read==0) break;   // nothing more buffered yet
        if(err!=ESP_OK) return -2;
        size_t frames=read/(2u*sizeof(int16_t));
        // The level for the on-screen indicator, taken here because this loop
        // already touches every sample: one compare each, no second pass.
        int loud=0;
        for(size_t i=0;i<frames;i++) {
            int v=scratch[i*2];
            if(v<0) v=-v;
            if(v>loud) loud=v;
        }
        if(loud>=32767) atomic_store(&clip_us,(int)esp_timer_get_time());
        unsigned was=atomic_load(&level_peak);
        atomic_store(&level_peak,(unsigned)loud>was?(unsigned)loud:was);
        atomic_store(&level_us,(int)esp_timer_get_time());
        // Slot 0, and taking the other one would do just as well: the codec is
        // mono and fills both halves of the frame with the same sample. That is
        // measured, not assumed -- sound_capture_probe() reported the two slots
        // separately and they agreed to the digit (L peak=227 mean=-23,
        // R peak=227 mean=-23, 2026-09-08). So there is no "we are reading the
        // empty half" bug hiding here, and no reason to look again.
        for(size_t i=0;i<frames;i++) out[got+(int)i]=scratch[i*2];
        got+=(int)frames;
        if(read<want) break;
    }
    // And after: the ring may have wrapped during the copy above, in which case
    // what was just copied straddles the hole. Refusing it costs one read and
    // keeps "time-contiguous" true of every array this ever returns.
    if(atomic_load(&overflowed)) return -1;
    atomic_fetch_add(&captured,(unsigned)got);
    return got;
}

// Integer square root of a mean square: no FPU, and verified against sqrt()
// over 200,000 values with no mismatch.
static unsigned rms_of(int64_t square, unsigned frames) {
    if(!frames) return 0;
    int64_t v=square/(int64_t)frames, r=0, bit=1LL<<30;
    while(bit>v) bit>>=2;
    while(bit) {
        if(v>=r+bit) { v-=r+bit; r=(r>>1)+bit; }
        else r>>=1;
        bit>>=2;
    }
    return (unsigned)r;
}

// The instrument for this codec's input.
//
// DRIVEN FROM THE SERIAL CONSOLE, not from the Cardputer's keyboard. Send the
// character '9' over USB (main.c reads it there). Pressing 9 on the device's
// own keys does nothing and looks exactly like a broken diagnostic -- it cost
// two attempts and several minutes the first time somebody tried.
//
// It profiles a recording in time, reading through sound_capture_read() -- the
// same call an app makes, polling and sleeping the way a guest reading once a
// frame does. Both of those are the result of eight runs whose tables
// contradicted each other, and both are corrections to this file:
//
//   * every earlier version read i2s_channel_read() directly, which is not the
//     product's path. A real recording through the API carried audio, twice,
//     while the probe reported silence, and that was the only structural
//     difference between them. An instrument that does not exercise the path
//     under test can only ever measure itself.
//
//   * every earlier version reported one number per recording. That hid the
//     thing the numbers were arguing about: a row reporting settle=2831 and
//     rms=0 had signal in its first 500 ms and silence afterwards -- a
//     recording that stops partway, invisible to any statistic averaged over
//     the whole of it. "The second recording is dead" may well be that same
//     event misread, since a real recording is 0.65 s and the probe measured
//     only after 500 ms of settling.
//
// So it prints one bucket per 150 ms in order, and the shape of a recording is
// something you look at rather than infer. What to look for:
//
//   * buckets that hold a level for the whole three seconds: a working
//     recording, and the gain question can finally be asked.
//   * buckets that go to zero and stay there: the input stops partway; the
//     bucket where it happens is when.
//   * SHORT on a bucket: fewer frames arrived than the window wanted, so the
//     stream is not keeping up -- a different fault from silence.
//   * OVERFLOW: sound_capture_read() reported the ring overran, which is the
//     product's own LIMIT_EXCEEDED path, and the profile stops there.
//
// Runs on a task of its own; see below for why that is not optional.
static bool probe_profile(const char *label, unsigned ms_total, unsigned ms_bucket) {
    if(!sound_capture_start()) {
        ESP_LOGW("sound","CAPTURE PROFILE %s could not start",label);
        return false;
    }
    unsigned alive=0;
    uint8_t have14=0xff, have17=0xff;
    codec_read(0x14,&have14);
    codec_read(0x17,&have17);
    ESP_LOGI("sound","CAPTURE PROFILE %s want=%02x/%02x have=%02x/%02x bucket=%ums",
             label,mic_reg14,mic_reg17,have14,have17,ms_bucket);
    int16_t buf[256];                    // mono frames, the caller's buffer
    unsigned per=ms_bucket*(unsigned)SAMPLE_RATE/1000u;
    for(unsigned t=0;t<ms_total;t+=ms_bucket) {
        int64_t square=0;
        unsigned frames=0, idle=0, clipped=0;
        int peak=0;
        bool overran=false;
        // The app's loop: ask, take what there is, sleep if there is none. The
        // idle cap keeps a stalled stream from hanging this task forever; a
        // bucket that hits it prints SHORT with the frames it did get.
        while(frames<per && idle<80) {
            int want=(int)(per-frames);
            if(want>(int)(sizeof buf/sizeof buf[0])) want=(int)(sizeof buf/sizeof buf[0]);
            int n=sound_capture_read(buf,want);
            if(n<0) { overran=true; break; }
            if(n==0) { idle++; vTaskDelay(pdMS_TO_TICKS(5)); continue; }
            for(int k=0;k<n;k++) {
                int v=buf[k];
                square+=(int64_t)v*v;
                if(v<0) v=-v;
                if(v>peak) peak=v;
                // Reported per bucket rather than per recording: a gain is
                // wrong if it clips at all, and a count averaged over three
                // seconds hides a syllable that hit the rail.
                if(v>=32767) clipped++;
            }
            frames+=(unsigned)n;
        }
        ESP_LOGI("sound","CAPTURE PROFILE %s t=%4u rms=%u peak=%d clip=%u frames=%u%s",
                 label,t,rms_of(square,frames),peak,clipped,frames,
                 overran?" OVERFLOW":(frames<per?" SHORT":""));
        // 32 of 32,767 is well under any microphone's noise floor and well over
        // a dead reader's stray bit; what it separates is "heard something"
        // from "heard nothing at all".
        if(peak>=32) alive++;
        if(overran) break;
    }
    sound_capture_stop();
    ESP_LOGI("sound","CAPTURE PROFILE %s buckets with signal: %u",label,alive);
    return alive>0;
}

// The analog microphone's level, which has to be established from nothing.
//
// Everything measured before 2026-09-08's last run was taken on the PDM pins,
// which carry a DC offset and no microphone, so every gain number this file
// ever had was a measurement of that offset on its way out through the ADC's
// high-pass. None of it transfers. The analog path has been heard exactly once,
// as ten buckets of varying level while somebody spoke, at 0x16=0x07 (the PGA
// at its maximum) and 0x17=0xa0:
//
//   2033  885  309  228  342  1312  587  1152  928  865
//
// That is speech and it is quiet -- an rms in the hundreds where a comfortable
// recording would sit in the low thousands -- so the sweep goes UP from here,
// and it moves both gains, because on the analog path both do something: 0x16
// is the PGA in front of the converter, 0x17 the digital volume behind it.
//
// The distinction matters more than the numbers. PGA gain is applied before
// quantisation and buys real signal-to-noise; digital gain is applied after and
// buys none, it only uses the int16 range. So the right answer is the most PGA
// that does not clip, with 0x17 making up whatever is left -- not whichever
// pair happens to hit a target level.
static const uint8_t EX_PGA5[][2]  = {{0x16,0x05}};
static const uint8_t EX_PGA6[][2]  = {{0x16,0x06}};

static void probe_gain(const char *label, uint8_t reg17,
                       const uint8_t (*extra)[2], unsigned n) {
    mic_reg17=reg17;
    extra_regs=extra; extra_count=n;
    probe_profile(label,1500,150);
    extra_regs=NULL; extra_count=0;
}

static void probe_run(void) {
    const uint8_t reg14=mic_reg14, reg17=mic_reg17;
    ESP_LOGI("sound","CAPTURE PROBE begin; speak into the microphone for ~10s");

    // Analog throughout: the path question is settled and the level question is
    // open. Read these by rms per bucket, and reject any row with clip above
    // zero however good its level looks.
    mic_reg14=0x1a;
    probe_gain("pga7-a0",0xa0,NULL,0);            // where the voice was heard
    probe_gain("pga7-b8",0xb8,NULL,0);            // +12 dB of digital volume
    probe_gain("pga7-c8",0xc8,NULL,0);            // esp-bsp's own value
    probe_gain("pga6-c8",0xc8,EX_PGA6,1);         // a step less PGA, same digital
    probe_gain("pga5-c8",0xc8,EX_PGA5,1);         // two steps less

    mic_reg14=reg14; mic_reg17=reg17;
    ESP_LOGI("sound","CAPTURE PROBE end; take the row with the most PGA and "
             "clip=0 throughout");
}

// The probe runs on a task of its own, and that is not a detail.
//
// It used to run inline on the ui task, which is the task that draws. Twelve
// seconds of measurement meant twelve seconds with no frame reaching the panel:
// the display froze on whatever had last been transferred, which looks exactly
// like a crash, and it is very probably the unexplained multi-minute blackout
// seen earlier the same day. sound_check_tables() gets away with running inline
// because it takes about 77 ms; the rule is not "diagnostics run inline", it is
// "nothing long-running runs on the task that draws".
//
// It also made the recording indicator untestable: the dot is composited into
// each strip as it is presented, so a task that stops presenting strips stops
// the indicator, and the one thing on screen that is meant to prove a recording
// is in progress could not appear during the only recording long enough to look
// at. That was noticed by somebody watching the glass rather than the log.
//
// Priority 4: below the drawing task at 5, so the display keeps its slot, and
// well below the audio task at 7. If the ring ever overruns because this task
// was starved, the row says so in its own ovf column rather than silently
// losing audio. The stack holds two 512-byte sample blocks and the log line.
static atomic_bool probing;

static void probe_task(void *arg) {
    (void)arg;
    probe_run();
    atomic_store(&probing,false);
    vTaskDelete(NULL);
}

void sound_capture_probe(void) {
    bool expected=false;
    if(!atomic_compare_exchange_strong(&probing,&expected,true)) {
        ESP_LOGW("sound","CAPTURE PROBE already running");
        return;
    }
    // While it runs, the codec is taken and pocket.audio.capture answers BUSY,
    // which is the same answer an app gets when another recording is live. That
    // is honest rather than special-cased: a recording IS live.
    if(xTaskCreate(probe_task,"micprobe",4096,NULL,4,NULL)!=pdPASS) {
        atomic_store(&probing,false);
        ESP_LOGW("sound","CAPTURE PROBE no room for its task");
    }
}

void sound_capture_stop(void) {
    if(!atomic_load(&capturing)) return;
    if(input) {
        i2s_channel_disable(input);
        i2s_del_channel(input);       // and with it the 6,144 bytes of DMA
        input=NULL;
    }
    codec_write(CAPTURE_OFF,sizeof CAPTURE_OFF/sizeof CAPTURE_OFF[0]);
    ESP_LOGI("sound","CAPTURE STOP frames=%u",atomic_load(&captured));
    // Last, so nothing queues playback into a codec that is still switching.
    atomic_store(&capturing,false);
}

static void audio_task(void *arg) {
    (void)arg;request_t req;int16_t pcm[256];
    while(1) {
        xQueueReceive(events,&req,portMAX_DELAY);
        // Claimed before the request is looked at and dropped after it is over,
        // so sound_capture_start() can tell "the queue is empty" from "the task
        // is halfway through the last thing it took off it".
        atomic_store(&playing,true);
        if(req.kind>=0) {
            if(req.kind<SFX_KINDS&&atomic_load(&enabled))play_click(req.kind,pcm);
        } else if(req.kind==-2) {
            play_stream(&req,pcm);
        } else {
            play_tone(&req,pcm);
        }
        atomic_store(&playing,false);
    }
}
void sound_init(i2c_master_bus_handle_t bus) {
    if(i2c_master_probe(bus,0x18,30)!=ESP_OK){ESP_LOGW("sound","ES8311 unavailable");return;}
    // Kept for the capture path, which powers the codec's ADC up and down
    // around a recording rather than leaving it on from boot.
    codec_bus=bus;
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
