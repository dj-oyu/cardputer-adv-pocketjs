// Run under ASan/UBSan. Decode the same upstream frames whole and through the
// project's converter; identity at 24 kHz and exact sample counts at all rates.
// Each file is decoded twice -- once through the eight-output block kernel
// (tools/host_fir_stub.c stands in for the assembly) and once through the scalar
// per-sample filter -- and the two must emit the same samples, because the kernel
// is bit-exact by construction. That is the caller-side half of the claim, on real
// frames, with no device involved.
#include "mp3_decode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t count, hash; int16_t *expected; unsigned at;
                 int16_t *got; size_t cap, n; } output_t;
static bool emit(void *ctx, int16_t value) {
    output_t *o=ctx;
    if(o->expected) assert(value==o->expected[o->at++]);
    if(o->got) {
        if(o->n==o->cap) { o->cap=o->cap?o->cap*2:4096; o->got=realloc(o->got,o->cap*2); assert(o->got); }
        o->got[o->n++]=value;
    }
    o->count++;
    o->hash=(o->hash^(uint16_t)value)*1099511628211ULL;
    return true;
}
int main(int argc, char **argv) {
    assert(argc>1);
    for(int a=1;a<argc;a++) {
        FILE *f=fopen(argv[a],"rb"); assert(f);
        fseek(f,0,SEEK_END); long size=ftell(f); rewind(f);
        uint8_t *data=malloc((size_t)size); assert(data);
        assert(fread(data,1,(size_t)size,f)==(size_t)size); fclose(f);
        pocket_mp3_decoder_t *d=malloc(sizeof(*d)); assert(d);
        int16_t decoded[2304];
        uint64_t count[2]={0,0}, hash[2]={0,0};
        unsigned rate=0, packets=0;
        output_t *arms[2]={calloc(1,sizeof(output_t)),calloc(1,sizeof(output_t))};
        assert(arms[0]&&arms[1]);
        for(int pass=0;pass<2;pass++) {
            g_mp3_fir_pie=pass==0;
            pocket_mp3_init(d,decoded);
            mp3dec_t ref; mp3dec_init(&ref);
            int16_t pcm[2304], mono[1152];
            output_t out={0};
            out.cap=4096; out.got=malloc(out.cap*2); assert(out.got); out.n=0;
            uint64_t input=0; unsigned pk=0;
            for(long at=0;at+4<=size;) {
                pocket_mp3_header_t h;
                if(!pocket_mp3_header(data+at,&h)) break;
                assert(at+h.bytes<=size);
                mp3dec_frame_info_t info={0};
                int n=mp3dec_decode_frame(&ref,data+at,(int)h.bytes,pcm,&info);
                if(n==0) break;
                assert(n==(int)h.samples);
                if(h.rate==24000) {
                    for(int i=0;i<n;i++) mono[i]=h.channels==1?pcm[i]:
                        (int16_t)(((int)pcm[2*i]+pcm[2*i+1])/2);
                    out.expected=mono; out.at=0;
                }
                assert(pocket_mp3_decode(d,data+at,h.bytes,emit,&out));
                input+=h.samples; rate=h.rate; pk++; at+=h.bytes;
                assert(out.count==input*24000/rate);
            }
            assert(pk>0);
            count[pass]=out.count; hash[pass]=out.hash; packets=pk;
            arms[pass]->count=out.count; arms[pass]->got=out.got; arms[pass]->n=out.n;
        }
        // Diagnostics before the assertion: where do the arms first disagree?
        {
            size_t m=arms[0]->n<arms[1]->n?arms[0]->n:arms[1]->n, diff=0, first=0, last=0;
            int shown=0;
            for(size_t i=0;i<m;i++)
                if(arms[0]->got[i]!=arms[1]->got[i]) {
                    if(!diff) first=i;
                    last=i; diff++;
                    if(shown<6) {
                        printf("  differ at %zu: block=%d scalar=%d (delta %d)\n", i,
                               arms[0]->got[i], arms[1]->got[i],
                               (int)arms[0]->got[i]-(int)arms[1]->got[i]);
                        shown++;
                    }
                }
            // Quiet when the two arms agree, which is the case that matters.
            if(diff)
                printf("MP3_DIAG %s rate=%u packets=%u samples block=%zu scalar=%zu differ=%zu first=%zu last=%zu\n",
                       argv[a],rate,packets,arms[0]->n,arms[1]->n,diff,first,last);
            fflush(stdout);
        }
        assert(count[0]==count[1]);
        assert(hash[0]==hash[1]);
        assert(packets>0);
        uint8_t bad[4]={255,251,0,0}; pocket_mp3_header_t h;
        assert(!pocket_mp3_header(bad,&h)); // free-format
        bad[2]=0xf0; assert(!pocket_mp3_header(bad,&h)); // reserved bitrate
        g_mp3_fir_pie=1;
        output_t dummy={0};
        assert(!pocket_mp3_decode(d,data,3,emit,&dummy));
        printf("MP3_OK %s rate=%u packets=%u output=%llu hash=%llu state=%zu fir=block==scalar\n",
               argv[a],rate,packets,(unsigned long long)count[0],
               (unsigned long long)hash[0],sizeof(*d));
        free(arms[0]->got); free(arms[1]->got); free(arms[0]); free(arms[1]);
        free(d); free(data);
    }
    return 0;
}
