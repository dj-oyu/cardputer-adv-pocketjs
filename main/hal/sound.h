#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"

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

// ------------------------------------------------------------------- clips
//
// A clip is a block of decoded-on-the-fly audio the caller already holds in
// RAM: 16-bit PCM, or IMA ADPCM in the WAV block layout. It plays through the
// same task, the same queue and the same I2S channel as the clicks and tones,
// which is the only reason it fits here at all -- see docs/common-api.md 9.1
// for why a real codec does not.
//
// Everything is mono at SOUND_SAMPLE_RATE. There is no resampler on this host,
// so a clip at any other rate is the caller's to refuse.
enum {
    SOUND_CLIP_PCM16 = 0,       // int16 little-endian, one channel
    SOUND_CLIP_IMA   = 1,       // IMA ADPCM, `block` bytes per block
};

#define SOUND_SAMPLE_RATE 24000u

// Queues one clip and returns its id, or one of the negative errors above.
// `data` must stay put and unchanged until the callback lands or
// sound_clip_stop() returns true -- the audio task reads it in place, which is
// what keeps a clip from needing a ring buffer of its own. `frames` is how many
// output samples the payload is worth; `block` is the ADPCM block size and is
// ignored for PCM16. A clip queued behind a tone waits for it, as everything
// on this queue does.
int32_t sound_clip_start(const uint8_t *data, uint32_t bytes, int format,
                         uint16_t block, uint32_t frames, float gain,
                         sound_done_fn done, void *ctx);

// Stops the clip with this id and waits for the audio task to let go of its
// buffer. True when it has -- and only then may the caller free those bytes.
// False means the wait ran out with the task still inside the clip, which
// leaves the caller no choice but to leak the buffer; it has not been seen.
bool sound_clip_stop(int32_t id);

// Output frames this clip has produced so far. Zero once it is over, so read it
// before the callback lands or keep your own total.
uint32_t sound_clip_position(int32_t id);

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
// and sound_tone()/sound_clip_start() answer SOUND_ERR_BUSY, which is also what
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
