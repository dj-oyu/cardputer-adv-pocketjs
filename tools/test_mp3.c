// Run under ASan/UBSan. Decode the same upstream frames whole and through the
// project's converter; identity at 24 kHz and exact sample counts at all rates.
#include "mp3_decode.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint64_t count, hash; int16_t *expected; unsigned at; } output_t;
static bool emit(void *ctx, int16_t value) {
    output_t *o=ctx;
    if(o->expected) assert(value==o->expected[o->at++]);
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
        pocket_mp3_init(d,decoded);
        mp3dec_t ref; mp3dec_init(&ref);
        int16_t pcm[2304], mono[1152];
        output_t out={0};
        uint64_t input=0; unsigned rate=0, packets=0;
        for(long at=0;at+4<=size;) {
            pocket_mp3_header_t h;
            if(!pocket_mp3_header(data+at,&h)) break;
            assert(at+h.bytes<=size);
            mp3dec_frame_info_t info={0};
            int n=mp3dec_decode_frame(&ref,data+at,(int)h.bytes,pcm,&info);
            // Some conformance streams intentionally start with a missing
            // reservoir. This player deliberately rejects those damaged files.
            if(n==0) break;
            assert(n==(int)h.samples);
            if(h.rate==24000) {
                for(int i=0;i<n;i++) mono[i]=h.channels==1?pcm[i]:
                    (int16_t)(((int)pcm[2*i]+pcm[2*i+1])/2);
                out.expected=mono; out.at=0;
            }
            assert(pocket_mp3_decode(d,data+at,h.bytes,emit,&out));
            input+=h.samples; rate=h.rate; packets++; at+=h.bytes;
            assert(out.count==input*24000/rate);
        }
        assert(packets>0);
        uint8_t bad[4]={255,251,0,0}; pocket_mp3_header_t h;
        assert(!pocket_mp3_header(bad,&h)); // free-format
        bad[2]=0xf0; assert(!pocket_mp3_header(bad,&h)); // reserved bitrate
        assert(!pocket_mp3_decode(d,data,3,emit,&out));
        printf("MP3_OK %s rate=%u packets=%u output=%llu hash=%llu state=%zu\n",
               argv[a],rate,packets,(unsigned long long)out.count,
               (unsigned long long)out.hash,sizeof(*d));
        free(d); free(data);
    }
    return 0;
}
