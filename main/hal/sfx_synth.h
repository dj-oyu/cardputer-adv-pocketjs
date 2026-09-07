#pragma once
// The synthesis sound_init() used to run on the device, kept verbatim after
// tools/make_sfx.py started baking its output into flash.
//
// It is here, and not deleted, because it is the thing the generator claims to
// reproduce: tools/test_sfx.py runs these two functions on the host and diffs
// them against the generated const tables, so "the generator produces what the
// device produced" is a measured number rather than a story. Nothing in the
// firmware calls it any more -- that is the point of the change -- so it lives
// in a header with no .c file and costs the image nothing.
//
// If either of these is edited, the transcription in tools/make_sfx.py has to
// move with it, and test_sfx.py is what notices if it did not.
#include <math.h>
#include <stdint.h>

enum { SFX_REF_KINDS=3, SFX_REF_RATE=24000,
       SFX_REF_WAVE_POINTS=256, SFX_REF_WAVE_PEAK=12000 };
static const int sfx_ref_frames[SFX_REF_KINDS]={720,1440,1080};

// Writes sfx_ref_frames[kind] samples of one click into out.
static void sfx_ref_synthesize(int kind,int16_t *out) {
    int frames=sfx_ref_frames[kind];
    float phase=0,frequency=kind==1?880:kind==2?440:660;
    for(int n=0;n<frames;n++) {
        float u=(float)n/frames;
        float envelope=fminf(n/72.0f,1.0f)*(1-u)*(1-u);
        phase+=6.2831853f*frequency*(kind==1?1+0.35f*u:1-0.15f*u)/SFX_REF_RATE;
        // Peak stays near -7 dBFS including the second harmonic.
        out[n]=(int16_t)(12000*envelope*(sinf(phase)+0.18f*sinf(phase*2)));
    }
}

// One period of a sine, the table play_tone() interpolates between.
static void sfx_ref_wave(int16_t *out) {
    for(int i=0;i<SFX_REF_WAVE_POINTS;i++)
        out[i]=(int16_t)(SFX_REF_WAVE_PEAK*sinf(6.2831853f*i/SFX_REF_WAVE_POINTS));
}
