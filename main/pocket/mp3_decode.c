#include "mp3_decode.h"
#include <math.h>
#include <string.h>

bool pocket_mp3_header(const uint8_t h[4], pocket_mp3_header_t *out) {
    static const unsigned rates[]={44100,48000,32000};
    static const unsigned br1[]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    static const unsigned br2[]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    unsigned version=(h[1]>>3)&3, layer=(h[1]>>1)&3;
    unsigned br=h[2]>>4, sr=(h[2]>>2)&3;
    if(h[0]!=255||(h[1]&224)!=224||version==1||layer!=1||
       br==0||br==15||sr==3||(h[3]&3)==2) return false;
    out->rate=rates[sr]>>(version==3?0:version==2?1:2);
    out->channels=(h[3]>>6)==3?1:2;
    out->samples=version==3?1152:576;
    out->bytes=(version==3?144000:72000)*(version==3?br1[br]:br2[br])/
               out->rate+((h[2]>>1)&1);
    return out->bytes>=24&&out->bytes<=2048;
}

void pocket_mp3_init(pocket_mp3_decoder_t *d, int16_t *pcm) {
    memset(d,0,sizeof(*d));
    mp3dec_init(&d->decoder);
    d->pcm=pcm;
}

// See mp3_decode.h: 1 uses the eight-output block kernel, 0 the scalar filter the
// rate converter's rounding was written against.
int g_mp3_fir_pie=1;

static void filter_init(pocket_mp3_decoder_t *d, unsigned rate) {
    // A 32-tap windowed sinc precedes downsampling. Linear interpolation alone
    // folds treble into the audible band at 44.1/48 kHz. Coefficients live in
    // the playback allocation, never in permanent DRAM.
    float taps[32], sum=0;
    float cutoff=10800.0f/(float)rate;
    for(unsigned i=0;i<32;i++) {
        float x=(float)i-15.5f;
        taps[i]=sinf(6.28318530718f*cutoff*x)/(3.14159265359f*x)*
                (0.54f-0.46f*cosf(6.28318530718f*i/31.0f));
        sum+=taps[i];
    }
    int total=0;
    for(unsigned i=0;i<32;i++) {
        d->filter[i]=(int16_t)lroundf(taps[i]*16384.0f/sum);
        total+=d->filter[i];
    }
    d->filter[15]+=(int16_t)(16384-total);
}

bool pocket_mp3_decode(pocket_mp3_decoder_t *d, const uint8_t *frame,
                      unsigned bytes, pocket_mp3_emit_t emit, void *ctx) {
    pocket_mp3_header_t h;
    if(bytes<4||!pocket_mp3_header(frame,&h)||h.bytes!=bytes||
       (d->rate&&d->rate!=h.rate)) return false;
    if(!d->rate) {
        d->rate=h.rate;
        if(h.rate>24000) filter_init(d,h.rate);
    }
    mp3dec_frame_info_t info={0};
    int n=mp3dec_decode_frame(&d->decoder,frame,(int)bytes,d->pcm,&info);
    if(n!=(int)h.samples||info.frame_bytes!=(int)bytes||info.layer!=3||
       info.hz!=(int)h.rate||info.channels!=(int)h.channels) return false;
    for(int i=0;i<n;i++) {
        // Eight outputs at a time when the kernel is switched on and the ring's phase
        // allows it (docs/perf/pie-opt-plan.md 6, A1/T3). The phase rule is the
        // kernel's: a block starts where cursor % 8 == 0, and that is what puts the
        // window on a 16-byte boundary. MPEG Layer III frames are 1152 or 576 samples
        // -- both multiples of eight -- so with the ring started at cursor 0 the block
        // path covers whole frames; anything else falls through to the scalar path
        // below.
        if(h.rate>24000&&g_mp3_fir_pie&&i+FIR_LANES<=n&&FIR_PHASE_OK(d->cursor)) {
            int16_t out8[FIR_LANES];
            for(int j=0;j<FIR_LANES;j++) {
                int s=d->pcm[(i+j)*h.channels];
                if(h.channels==2) s=(s+d->pcm[(i+j)*2+1])/2;
                fir_ring_push(d->history,(int)((d->cursor+(unsigned)j)%FIR_RING_SLOTS),(int16_t)s);
            }
            fir8_pie(fir_window(d->history,(int)d->cursor),d->filter,out8);
            d->cursor=(d->cursor+FIR_LANES)%FIR_RING_SLOTS;
            // The phase advance and its rounding are the per-sample path's, eight
            // times over, in the same order: keep the two copies in step.
            for(int j=0;j<FIR_LANES;j++) {
                int sample=out8[j];
                d->phase+=24000;
                while(d->phase>=h.rate) {
                    d->phase-=h.rate;
                    int out=sample+(int)((int64_t)(d->previous-sample)*d->phase/24000);
                    if(!emit(ctx,(int16_t)out)) return false;
                }
                d->previous=sample;
            }
            i+=FIR_LANES-1;
            continue;
        }
        int sample=d->pcm[i*h.channels];
        if(h.channels==2) sample=(sample+d->pcm[i*2+1])/2;
        if(h.rate>24000) {
            fir_ring_push(d->history,(int)d->cursor,(int16_t)sample);
            int32_t sum=0;
            for(unsigned k=0;k<32;k++)
                // The ring is not a power of two any more, so the wrap has to be added
                // before the modulo: cursor - k alone is an unsigned underflow.
                sum+=(int32_t)d->history[(d->cursor+FIR_RING_SLOTS-k)%FIR_RING_SLOTS]*d->filter[k];
            d->cursor=(d->cursor+1)%FIR_RING_SLOTS;
            // Floor, not truncation toward zero: an arithmetic shift is what the
            // PIE accumulator readout (EE.SRS.ACCX) computes, so a vector version
            // of this loop can match it bit for bit. It differs from sum/16384
            // only for a negative sum with a remainder, by one LSB, and costs
            // four fewer instructions a sample on this core.
            sample=sum>>14;
            if(sample>32767) sample=32767;
            if(sample< -32768) sample= -32768;
        }
        // Integer phase survives frame boundaries, so long VBR files cannot
        // accumulate sample-count rounding drift. At 24 kHz this is identity.
        d->phase+=24000;
        while(d->phase>=h.rate) {
            d->phase-=h.rate;
            int out=sample+(int)((int64_t)(d->previous-sample)*d->phase/24000);
            if(!emit(ctx,(int16_t)out)) return false;
        }
        d->previous=sample;
    }
    return true;
}

// ------------------------------------------------------------- the frame count

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

uint32_t pocket_mp3_total_frames(const uint8_t *first, unsigned len,
                                 const pocket_mp3_header_t *header) {
    if(!first||!header) return 0;
    // Where the tag sits depends on the side information, whose size is fixed
    // by the version and the channel mode. samples==1152 is MPEG-1; 576 is
    // MPEG-2 or 2.5, which halve it.
    unsigned side = header->samples==1152 ? (header->channels==1?17u:32u)
                                          : (header->channels==1?9u:17u);
    unsigned at = 4u+side;
    if(at+8<=len && (!memcmp(first+at,"Xing",4)||!memcmp(first+at,"Info",4))) {
        uint32_t flags=be32(first+at+4);
        // Bit 0 is the frame count. The other three fields (bytes, the seek
        // table, quality) are laid out after it in flag order, and none of them
        // is wanted here -- the seek table is 100 one-byte fractions, which is
        // far too coarse to seek with and is not why this is being read.
        if(!(flags&1u)) return 0;
        if(at+12>len) return 0;
        return be32(first+at+8);
    }
    // VBRI sits at a fixed offset instead of after the side information, and is
    // Fraunhofer's rather than the LAME lineage's. Rare, cheap to check.
    if(4u+32u+18u<=len && !memcmp(first+4+32,"VBRI",4))
        return be32(first+4+32+14);
    return 0;
}
