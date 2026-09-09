// The Xing/Info/VBRI reader, on the host.
//
// WHY THIS EXISTS RATHER THAN A BOARD RUN. The album that prompted the feature
// has no tag in any of its twenty files, so on that card the parser returns 0
// every time and looks correct while doing nothing. A test that only ever
// exercises "absent" cannot tell a working reader from a stub, and the branch
// that matters -- a file that DOES state its length -- would have shipped
// unexecuted.
//
//   gcc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined
//       -I main/pocket -I .cache/codecs/minimp3 tools/test_mp3_duration.c
//       main/pocket/mp3_decode.c components/minimp3/minimp3.c -lm -o /tmp/t
//   && /tmp/t
//
// (from the repository root)

#include "mp3_decode.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond,...) do { if(!(cond)) { \
    printf("FAIL %s:%d ",__FILE__,__LINE__); printf(__VA_ARGS__); printf("\n"); \
    failures++; } } while(0)

// A frame's first four bytes. MPEG-1 Layer III, 128 kbps, 44.1 kHz, and the
// channel mode in the top two bits of byte 3.
static void header(uint8_t *h, int mono) {
    h[0]=0xFF; h[1]=0xFB; h[2]=0x90; h[3]=mono?0xC0:0x00;
}

static void put32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16);
    p[2]=(uint8_t)(v>>8);  p[3]=(uint8_t)v;
}

int main(void) {
    uint8_t buf[64];
    pocket_mp3_header_t h;

    uint8_t hd[4];
    header(hd,0);
    CHECK(pocket_mp3_header(hd,&h),"stereo header rejected");
    CHECK(h.channels==2,"stereo read as %u channels",h.channels);
    CHECK(h.samples==1152,"MPEG-1 frame is %u samples",h.samples);
    CHECK(h.rate==44100,"rate read as %u",h.rate);

    // ---- Xing, stereo: the tag sits after 32 bytes of side information -------
    memset(buf,0,sizeof buf);
    memcpy(buf,hd,4);
    memcpy(buf+4+32,"Xing",4);
    put32(buf+4+32+4,0x0001);          // flags: frame count present
    put32(buf+4+32+8,9000);            // frames
    CHECK(pocket_mp3_total_frames(buf,sizeof buf,&h)==9000,
          "stereo Xing gave %u",pocket_mp3_total_frames(buf,sizeof buf,&h));

    // "Info" is the same layout and is what a CBR encoder writes instead.
    memcpy(buf+4+32,"Info",4);
    CHECK(pocket_mp3_total_frames(buf,sizeof buf,&h)==9000,"Info not accepted");

    // The flag is not decoration. Without bit 0 the four bytes that follow are
    // whatever field IS present, and reading them as a count would invent a
    // length out of a byte total -- the failure this whole feature exists to
    // avoid, arrived at from the other side.
    put32(buf+4+32+4,0x0006);
    CHECK(pocket_mp3_total_frames(buf,sizeof buf,&h)==0,
          "a tag without the frames flag must give nothing");

    // ---- mono moves the tag: 17 bytes of side information, not 32 -----------
    uint8_t mono_hd[4];
    header(mono_hd,1);
    pocket_mp3_header_t m;
    CHECK(pocket_mp3_header(mono_hd,&m),"mono header rejected");
    CHECK(m.channels==1,"mono read as %u channels",m.channels);
    memset(buf,0,sizeof buf);
    memcpy(buf,mono_hd,4);
    memcpy(buf+4+17,"Xing",4);
    put32(buf+4+17+4,0x0001);
    put32(buf+4+17+8,1234);
    CHECK(pocket_mp3_total_frames(buf,sizeof buf,&m)==1234,"mono Xing missed");
    // And the stereo offset must NOT find it, or the two layouts would be one.
    CHECK(pocket_mp3_total_frames(buf,sizeof buf,&h)==0,
          "the stereo offset found a mono tag");

    // ---- VBRI: a fixed offset, not one that follows the side information ----
    memset(buf,0,sizeof buf);
    memcpy(buf,hd,4);
    memcpy(buf+4+32,"VBRI",4);
    put32(buf+4+32+14,4321);
    CHECK(pocket_mp3_total_frames(buf,sizeof buf,&h)==4321,"VBRI missed");

    // ---- no tag at all, which is the album that prompted this ---------------
    memset(buf,0,sizeof buf);
    memcpy(buf,hd,4);
    CHECK(pocket_mp3_total_frames(buf,sizeof buf,&h)==0,"invented a length");

    // ---- a buffer too short to hold the tag ---------------------------------
    memset(buf,0,sizeof buf);
    memcpy(buf,hd,4);
    memcpy(buf+4+32,"Xing",4);
    put32(buf+4+32+4,0x0001);
    put32(buf+4+32+8,9000);
    for(unsigned len=0;len<48;len++)
        CHECK(pocket_mp3_total_frames(buf,len,&h)==0,
              "read a tag out of %u bytes",len);
    CHECK(pocket_mp3_total_frames(buf,48,&h)==9000,"48 bytes should be enough");

    // ---- the arithmetic the player does with the answer ----------------------
    // 9000 frames of 1152 samples at 44.1 kHz is 235.102... seconds. The player
    // rounds rather than truncates, because a bar that stops a millisecond
    // short of the end reads as a bug rather than as a rounding.
    uint32_t ms=(uint32_t)(((uint64_t)9000*1152*1000u+44100/2)/44100);
    CHECK(ms==235102,"235.102 s came out as %u ms",ms);

    CHECK(pocket_mp3_total_frames(NULL,64,&h)==0,"NULL buffer");
    CHECK(pocket_mp3_total_frames(buf,64,NULL)==0,"NULL header");

    printf(failures?"FAILURES %d\n":"ok\n",failures);
    return failures?1:0;
}
