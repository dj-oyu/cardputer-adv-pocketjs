#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include "driver/i2c_master.h"
#include "sound_stream.h"

// sound_tone's limits. The ceiling is well under the 12 kHz Nyquist of the
// 24 kHz output so the wavetable's own aliasing stays inaudible; the floor is
// where the speaker stops producing anything. Neither has been measured on the
// physical speaker, so they are guards against nonsense rather than a claim
// about what the hardware reproduces.
#define SOUND_TONE_MIN_HZ 20u
#define SOUND_TONE_MAX_HZ 8000u
#define SOUND_TONE_MAX_MS 5000u

// Negative results from sound_tone. A success is a positive id, never zero.
enum {
    SOUND_ERR_UNSUPPORTED = -1,   // no codec on this board, or audio failed to start
    SOUND_ERR_INVALID     = -2,   // frequency, duration or gain out of range
    SOUND_ERR_BUSY        = -3,   // the play queue is full
};

void sound_init(i2c_master_bus_handle_t bus);
void sound_set_enabled(bool enabled);

// The DAC's output level, 0..SOUND_VOLUME_STEPS-1.
//
// A DEVICE PROPERTY, NOT AN APP'S. There is no pocket.audio.volume and there
// should not be: how loud this machine is belongs to the person holding it, not
// to whichever program is running, and an app that could turn itself up is an
// app that can be turned up while nobody is looking at it.
//
// It writes ES8311 register 0x32, which the init table has held at a constant
// 0xbf since the codec was brought up -- so the hardware could always do this
// and only the path was missing. Safe from any task; the codec device handle is
// added and removed around the write, as everything else here does.
#define SOUND_VOLUME_STEPS 5
void sound_set_volume(unsigned step);
unsigned sound_volume(void);
// Queues one of the three clicks. False if sound is muted or the queue is full,
// which is what section 9 asks audio.cue to report; it never means the click
// was heard, only that it was accepted. Existing callers ignore the result.
bool sound_play(int kind);
// False if sound_init found no codec; every other call here is then inert.
bool sound_available(void);

// Re-runs the synthesis the baked tables replaced and logs the worst absolute
// difference. A one-shot diagnostic ('8' over USB, main.c), not on any path a
// user reaches: the tables are in flash and the point is to find out what this
// board's newlib sinf says about them, which no host can answer. Allocates
// SFX_SAMPLES*2 bytes for the length of the check and frees them.
void sound_check_tables(void);

// Called from the audio task once, when the tone stops. completed is false if
// sound_tone_cancel got to it first, or if the I2S write failed. The callback
// runs at the audio task's priority with its stack, so it must do no more than
// hand the result to whichever loop is waiting for it.
typedef void (*sound_done_fn)(void *ctx, bool completed);

// Queues one tone and returns its id, or one of the negative errors above. gain
// is 0..1 and scales the same peak the clicks use. Tones and clicks share one
// queue and so play one after another, never mixed; a tone therefore delays any
// click queued behind it by up to its own duration. If sound is muted the tone
// still occupies its full duration in silence, so an app using tones for timing
// keeps its timing -- muting changes what is heard, not when.
int32_t sound_tone(unsigned frequency_hz, unsigned duration_ms, float gain,
                   sound_done_fn done, void *ctx);
// Stops the tone with this id, whether it is playing or still queued, and makes
// its callback report completed=false. Only the most recent request to cancel
// is remembered, which is enough while one tone plays at a time.
void sound_tone_cancel(int32_t id);

// ------------------------------------------------------------------ streams
//
// A stream is audio the caller produces while it plays: 16-bit PCM, or IMA
// ADPCM in the WAV block layout, arriving in slots the caller fills from its
// own task. It plays through the same task, the same queue and the same I2S
// channel as the clicks and tones.
//
// It replaced a "clip" that was the whole sound in one buffer, read in place.
// That shape made the buffer the limit on length -- 24,576 bytes, about 2.05 s
// of ADPCM -- and it made the buffer's size the cost of having a player open at
// all. Here the ring is what is resident and the source is what is long, so
// length is the source's business and the memory is a constant.
//
// Everything is mono at SOUND_SAMPLE_RATE. There is no resampler on this host,
// so a source at any other rate is the caller's to refuse.
enum {
    SOUND_STREAM_PCM16 = 0,     // int16 little-endian, one channel
    SOUND_STREAM_IMA   = 1,     // IMA ADPCM, `block` bytes per block
};

#define SOUND_SAMPLE_RATE 24000u

// sound_stream_t, the slot geometry, and the producer's three calls
// (sound_stream_slot / sound_stream_publish / sound_stream_rewind) are in
// sound_stream.h, which has no ESP dependency so that tools/test_stream.py can
// compile the same lines the audio task runs.

// What happens when the producer cannot keep up, which is the failure this
// design actually has. The audio task fills the block with silence, counts it,
// and DOES NOT ADVANCE the source: an underrun stretches the sound and never
// drops or repeats a sample of it. Nothing is invented and nothing is lost.
//
// Why not the alternatives. Repeating the last block invents audio that was
// never in the source. Skipping ahead to hold wall-clock timing loses audio
// silently, which is the playback twin of splicing a recording across a DMA
// overflow -- and section 9 already refuses that on the capture side, in the
// same words, for the same reason. Failing on the first late block would end
// playback over one slow frame, and the producer runs on the task that draws,
// which has been measured at 39.9 ms against a 33 ms budget; being late
// occasionally is this system's normal condition, not its exception.
//
// What is NOT survivable is a producer that has stopped altogether -- a session
// ending with a stream queued, or a source that went away. After this long with
// nothing published at all, the stream ends incomplete and the app is told, as
// it is told about a failed I2S write. Sounding silence forever is worse than
// saying so.
#define SOUND_STREAM_STARVE_BLOCKS 512
#define SOUND_STREAM_STARVE_MS \
    ((SOUND_STREAM_STARVE_BLOCKS*128*1000)/(int)SOUND_SAMPLE_RATE)

// Queues one stream and returns its id, or one of the negative errors above.
// `frames` is how many output samples the source is worth and bounds the
// playback; `block` is the ADPCM block size and is ignored for PCM16. At least
// one slot should be published before this is called, or the first block is an
// underrun. A stream queued behind a tone waits for it, as everything on this
// queue does.
int32_t sound_stream_start(sound_stream_t *stream, int format, uint16_t block,
                           uint32_t frames, float gain,
                           sound_done_fn done, void *ctx);

// Stops the stream with this id and waits for the audio task to let go of the
// ring. True when it has -- and only then may the caller free those bytes.
bool sound_stream_stop(int32_t id);

// Output frames this stream has produced so far. Zero once it is over, so read
// it before the callback lands or keep your own total.
uint32_t sound_stream_position(int32_t id);

// 128-frame blocks the audio task had to fill with silence because the producer
// had published nothing. This is the number `status().underruns` reports, and
// with a streamed source it is no longer always zero. Cleared at each start.
uint32_t sound_stream_underruns(void);

// ------------------------------------------------------------------ capture
//
// The microphone, which reaches the chip the same way the speaker leaves it:
// through the ES8311 over I2S_NUM_1. It is not a peripheral of its own on this
// board -- ASDOUT (GPIO46) is the codec's ADC data, and BCLK and WS are the
// clock this file already drives for playback.
//
// The microphone is a PDM part reaching the codec on its DMIC pins, not an
// analog one on its microphone input -- measured, see mic_reg14 in sound.c.
//
// That one shared clock is why capture runs at SOUND_SAMPLE_RATE and not at
// section 9's 16000. IDF constitutes full duplex on a controller only when the
// second channel's clock and slot configuration match the first's exactly
// (i2s_std.c, "Constitude full-duplex on port %d"); differ, and the RX channel
// stops sharing BCLK/WS and tries to drive pins TX has already reserved. One
// controller has one sample rate, and the DAC's is 24000.
//
// Capture and playback are exclusive here, as docs/common-api.md section 9 asks
// of this first version: while a recording is open sound_play() answers false
// and sound_tone()/sound_stream_start() answer SOUND_ERR_BUSY, which is also what
// makes "UI cues do not sound while recording" a property of this file rather
// than a rule every caller has to remember.

// Frames one call to sound_capture_read() may ask for. Not a buffer size --
// the caller's -- but the surface publishes it, so it is stated once here.
#define SOUND_CAPTURE_MAX_FRAMES 2048

// Opens the RX channel and the codec's input path. False when there is no
// codec, when a recording is already open, or when the audio task is playing
// something (a click, a tone or a clip, queued or sounding).
//
// The channel is created here and deleted in sound_capture_stop(), so the DMA
// buffers -- 6 descriptors of 256 stereo frames, 6,144 bytes -- exist only
// while a recording does. On a board with no PSRAM that is the difference
// between a feature that costs an app 6 KiB and one that costs it nothing.
//
// That property was given up for an afternoon and taken back: see the comment
// in sound.c for the hypothesis that cost it, and for what an app doing
// open/close/open three times actually reports.
bool sound_capture_start(void);

// Copies up to max_frames of mono 16-bit PCM out of the DMA ring and returns
// how many frames it wrote; 0 means nothing has arrived yet. Negative results:
//
//   -1  the DMA overwrote audio nobody had read yet. The recording has a hole
//       in it, and section 9 refuses to splice across one silently, so the
//       caller's only move is to report it and close. Reported by the driver's
//       own on_recv_q_ovf callback, not inferred from timing.
//   -2  no recording is open, or the read failed.
//
// Contiguous by construction: the DMA ring is the only buffer, and everything
// it holds is handed over in order. Nothing here can skip a frame without the
// overflow above having fired.
int sound_capture_read(int16_t *out, int max_frames);

// Closes the recording and gives the DMA buffers back. Idempotent.
void sound_capture_stop(void);

// Whether a recording is open. The recording indicator on screen is drawn from
// this, so it is true for exactly as long as the microphone is live.
bool sound_capture_active(void);

// Whether the DMA has overrun unread audio since this was last asked, and
// clears the flag. sound_capture_read() consumes the same flag to fail a
// read with -1, so this is for the diagnostic, which reads the ring itself.
bool sound_capture_overflowed(void);

// The level of the audio most recently read, for the recording indicator, and
// whether anything hit the rail in the last second. `peak` is 0..32767 and
// fades to zero over 150ms if nothing is being read -- so an app that stops
// reading stops claiming a level, rather than leaving the last one on screen.
// Safe from any task; the drawing task is the caller that matters.
void sound_capture_level(unsigned *peak, bool *clipping);

// The microphone diagnostic: sweeps the codec's two input paths and then the
// ADC's digital volume, logging peak and mean for both I2S slots of each.
// **Sent as '9' over the USB serial console, not typed on the Cardputer's own
// keyboard** -- the device keys do not reach it, which looks identical to a
// diagnostic that does not work. It is what established that
// this board's MEMS microphone is PDM: the analog path measured a peak of 9
// over 4,864 frames and the PDM path 227, in the same quiet room. The full
// reading -- what a healthy row looks like, and what too little or too much
// gain looks like -- is above the function in sound.c. Takes the codec for
// about twelve seconds -- ON A TASK OF ITS OWN. It used to run inline on the
// task that draws, which froze the panel for the whole run and made the
// recording indicator impossible to see during the only recording long
// enough to look at. Returns as soon as the task is started.
void sound_capture_probe(void);

// Frames taken out of the DMA ring since sound_capture_start(). For the CAPTURE
// STOP log line and for tests; it counts what was read out, not what the codec
// produced -- and every reader counts, the diagnostic sweep included, so the
// marker reports the recording that actually happened rather than only the ones
// that went through sound_capture_read().
uint32_t sound_capture_frames(void);
