// The stream-side container walk, on a host, against a file make_opus_asset.c
// wrote.
//
//   wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs &&
//     gcc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined
//         -I main/hal -I main/pocket tools/test_opus_stream.c -o /tmp/t &&
//     /tmp/t apps/opusplay/tone.pok"
//
// It compiles main/pocket/opus_net.h -- opus_net_resume/cut/advance, the same
// three lines the receive task runs -- for the same reason test_stream.c
// compiles sound_stream.h and test_opus_pak.c compiles opus_feed.h: this is the
// part whose mistakes are SILENT. A length prefix read one byte late is a packet
// the decoder refuses, not a crash, and on the device that arrives as a stream
// that stops early with no explanation.
//
// What is new here, and what the file path never had to face: bytes arrive in
// whatever sizes the socket feels like. A file read returns what you asked for;
// a socket returns one byte, then 1,400, then nothing for a while. So the test
// replays the same asset through every awkward chunk size it can and demands
// that the published packet sequence is IDENTICAL to the file's every time.
//
// It also drives three rotating slots rather than one buffer, because the
// receiver deliberately keeps a partial packet in the slot it already published
// instead of paying for a carry buffer. If that aliasing is wrong, only a
// rotating ring shows it.
#include "opus_net.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...) do { if(!(cond)) { \
    printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while(0)

static uint8_t *slurp(const char *path, uint32_t *len) {
    FILE *f=fopen(path,"rb");
    if(!f) { perror(path); exit(2); }
    fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET);
    uint8_t *b=malloc((size_t)n);
    if(!b||fread(b,1,(size_t)n,f)!=(size_t)n) { perror(path); exit(2); }
    fclose(f);
    *len=(uint32_t)n;
    return b;
}

// The reference: every packet in the file, in order, walked straight off the
// length prefixes with no slots involved.
typedef struct { uint32_t off, len; } pkt_t;

static pkt_t *walk(const uint8_t *file, const opus_pak_t *k, uint32_t *count) {
    pkt_t *out=malloc(sizeof(pkt_t)*k->packet_count);
    uint32_t at=k->data_offset, n=0;
    while(at+2<=k->file_bytes && n<k->packet_count) {
        uint32_t len=(uint32_t)file[at]|((uint32_t)file[at+1]<<8);
        if(!len||at+2+len>k->file_bytes) break;
        out[n].off=at+2; out[n].len=len;
        n++; at+=2+len;
    }
    *count=n;
    return out;
}

// One replay: the body handed over `chunk` bytes at a time (0 means "a varying
// size that keeps landing inside packets"), through three rotating slots, using
// the receiver's own inline calls.
static void replay(const char *name, const uint8_t *file, const opus_pak_t *k,
                   const pkt_t *want, uint32_t want_n, uint32_t chunk) {
    static uint8_t slots[SOUND_STREAM_SLOTS][SOUND_STREAM_SLOT_BYTES];
    opus_net_carry_t carry={0};
    uint32_t src=k->data_offset;            // where the "socket" has got to
    uint32_t got_n=0, slot_i=0, published=0;
    uint32_t vary=1;
    bool eof=false, ended=false;

    while(!ended) {
        uint8_t *slot=slots[slot_i%SOUND_STREAM_SLOTS];
        uint32_t have=opus_net_resume(&carry,slot);
        while(have<SOUND_STREAM_SLOT_BYTES) {
            uint32_t take=chunk?chunk:vary;
            if(!chunk) { vary=vary*7+3; if(vary>777) vary%=777; if(!vary) vary=1; }
            if(take>SOUND_STREAM_SLOT_BYTES-have) take=SOUND_STREAM_SLOT_BYTES-have;
            uint32_t left=k->file_bytes-src;
            if(take>left) take=left;
            if(!take) { eof=true; break; }
            memcpy(slot+have,file+src,take);
            src+=take; have+=take;
        }
        opus_net_cut_t cut=opus_net_cut(slot,have,eof);
        if(!cut.publish) { ended=true; break; }
        published++;
        // What the decode task would see: the slot walked packet by packet.
        uint32_t at=0;
        while(at+2<=cut.publish) {
            uint32_t len=(uint32_t)slot[at]|((uint32_t)slot[at+1]<<8);
            if(at+2+len>cut.publish) {
                CHECK(0,"%s chunk=%u: a slot ended inside a packet",name,chunk);
                ended=true; break;
            }
            if(got_n>=want_n) {
                CHECK(0,"%s chunk=%u: more packets came out than went in",name,chunk);
                ended=true; break;
            }
            if(len!=want[got_n].len||memcmp(slot+at+2,file+want[got_n].off,len)) {
                CHECK(0,"%s chunk=%u: packet %u differs (len %u vs %u)",
                      name,chunk,got_n,len,want[got_n].len);
                ended=true; break;
            }
            // The gate the device applies to every packet, applied here too: a
            // container walk that produced the right bytes but a TOC the decoder
            // would refuse is not a pass.
            CHECK(opus_pak_toc_ok(slot[at+2]),
                  "%s chunk=%u: packet %u would be refused by the gate",
                  name,chunk,got_n);
            got_n++;
            at+=2+len;
        }
        if(cut.last) ended=true;
        opus_net_advance(&carry,slot,cut);
        slot_i++;
    }
    CHECK(got_n==want_n,"%s chunk=%u: %u packets out of %u",name,chunk,got_n,want_n);
    if(!failures) printf("  chunk %-5u  %u packets in %u slots\n",chunk,got_n,published);
}

int main(int argc, char **argv) {
    if(argc<2) { printf("usage: %s <file.pok>...\n",argv[0]); return 2; }
    for(int a=1;a<argc;a++) {
        uint32_t size=0;
        uint8_t *file=slurp(argv[a],&size);
        opus_pak_t k;
        const char *bad=opus_pak_parse(file,size,&k);
        if(bad) { printf("%s: %s\n",argv[a],bad); failures++; free(file); continue; }
        uint32_t want_n=0;
        pkt_t *want=walk(file,&k,&want_n);
        printf("%s: %u packets, max %u bytes\n",argv[a],want_n,k.max_packet);
        CHECK(want_n==k.packet_count,"the file holds %u packets, its header says %u",
              want_n,k.packet_count);
        // 1 is the pathological socket: every read lands inside a packet, and
        // every slot boundary has to be carried. 2048 is a socket that happens
        // to align with the slot. 0 is a size that keeps changing, which is what
        // a real one does.
        static const uint32_t CHUNKS[]={1,2,3,7,64,91,512,1400,2047,2048,0};
        for(unsigned i=0;i<sizeof CHUNKS/sizeof CHUNKS[0];i++)
            replay(argv[a],file,&k,want,want_n,CHUNKS[i]);
        free(want);
        free(file);
    }
    printf(failures?"%d check(s) failed\n":"all checks passed\n",failures);
    return failures?1:0;
}
