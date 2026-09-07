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
