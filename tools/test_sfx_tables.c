// Diffs the flash tables tools/make_sfx.py emits against the synthesis the
// device used to run (main/hal/sfx_synth.h). Run it through tools/test_sfx.py,
// which generates the header first; this file cannot be compiled alone.
//
// Exact equality is not the bar and is not reachable: the generator computes
// sinf in Python's double-precision libm and rounds, the reference computes it
// in the host's float libm, and the device computed it in newlib's. What the
// bar is, is a difference small enough to be inaudible, and this prints it so
// that the claim carries a number.
#include "sfx_synth.h"
#include "sfx_tables.h"
#include <stdio.h>
#include <stdlib.h>

// 32767 full scale; the clicks peak near 12000. One LSB is -90 dBFS.
#define TOLERANCE 2

static int worst(const char *what,const int16_t *a,const int16_t *b,int n) {
    int peak=0,at=0,differing=0;
    for(int i=0;i<n;i++) {
        int d=a[i]-b[i];if(d<0)d=-d;
        if(d){differing++;if(d>peak){peak=d;at=i;}}
    }
    printf("%-12s n=%4d differ=%4d worst=%d at %d\n",what,n,differing,peak,at);
    return peak;
}

int main(void) {
    int peak=0;
    if((int)SFX_KINDS!=(int)SFX_REF_KINDS){printf("kind count moved\n");return 1;}
    int total=0;
    for(int k=0;k<SFX_KINDS;k++) {
        if(sfx_frames[k]!=sfx_ref_frames[k]){printf("frame count moved\n");return 1;}
        if(sfx_offset[k]!=total){printf("offset table does not pack\n");return 1;}
        total+=sfx_frames[k];
    }
    if(total!=SFX_SAMPLES){printf("flat table is not the sum of the parts\n");return 1;}

    int16_t *reference=malloc(SFX_SAMPLES*sizeof(int16_t));
    if(!reference)return 1;
    for(int k=0;k<SFX_KINDS;k++) {
        sfx_ref_synthesize(k,reference+sfx_offset[k]);
        char label[16];snprintf(label,sizeof label,"click %d",k);
        int d=worst(label,sfx_pcm+sfx_offset[k],reference+sfx_offset[k],sfx_frames[k]);
        if(d>peak)peak=d;
    }
    free(reference);

    int16_t ref_wave[WAVE_POINTS];
    sfx_ref_wave(ref_wave);
    int d=worst("wave",wave,ref_wave,WAVE_POINTS);
    if(d>peak)peak=d;

    // The rectangle the device carried was 3*1440 samples; the flat table is
    // the sum of the real lengths. Both numbers are printed so the saving is
    // stated rather than asserted.
    printf("bytes: rectangle %d flat %d wave %d\n",
           SFX_KINDS*1440*2,SFX_SAMPLES*2,WAVE_POINTS*2);
    if(peak>TOLERANCE){printf("SFX_TABLES_FAIL worst=%d > %d\n",peak,TOLERANCE);return 1;}
    printf("SFX_TABLES_OK worst sample difference %d of 32767\n",peak);
    return 0;
}
