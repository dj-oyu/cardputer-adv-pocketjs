// The container walk, on a host, against a file tools/make_opus_asset.c wrote.
//
//   wsl -e bash -lc "cd /mnt/c/devs/m5stack/cardputer-adv-pocketjs &&
//     gcc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined
//         -I main/hal -I main/pocket tools/test_opus_pak.c -o /tmp/t &&
//     /tmp/t apps/chime/chime.pok""
//
// It compiles main/pocket/opus_feed.h -- the same lines the firmware runs -- for
// the same reason tools/test_stream.c compiles sound_stream.h: this is the part
// of playback whose mistakes are silent. A length prefix read one byte late is
// not a crash, it is a decoder that refuses a packet three slots into a clip.
//
// IT TAKES A REAL FILE ON THE COMMAND LINE, AND THAT IS THE DESIGN. The cheap
// version of this test builds packet structures by hand and checks the walk
// against them, which proves the walk works ON INPUTS THE TEST INVENTED and
// says nothing about the file the device will open. That failure mode --
// a check that verifies its own input rather than the real result -- is the
// most common way a green test coexists with a broken feature in this project.
// So the argument is the shipped asset, and what is checked is that the walk,
// the gate and the index agree with what the generator actually wrote.
//
// What it checks, and each one is a thing that has to hold on the device:
//
//   1. the header parses and describes CELT 20 ms mono at 24 kHz
//   2. every packet passes the gate the decode task applies (opus_pak_toc_ok)
//   3. THE SLICING IS INVISIBLE: walking the file in 2,048-byte slots the way
//      player_feed_opus() does, with opus_pak_whole() trimming each slot to
//      whole packets, yields exactly the packet sequence a straight walk does.
//      That is the container's version of "a source cut into slots decodes to
//      the same samples", which is what test_stream.c proves for the ring.
//   4. every index entry lands on a packet boundary, at the packet number the
//      interval says -- which is what seek() depends on and nothing else checks
//   5. the header's totalFrames is consistent with the packets present
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "opus_feed.h"

static int fails;
#define CHECK(cond,...) do{ if(!(cond)){ fails++; \
    printf("FAIL "); printf(__VA_ARGS__); printf("\n"); } }while(0)

int main(int argc, char **argv) {
    if(argc<2) { fprintf(stderr,"usage: %s FILE.pok\n",argv[0]); return 2; }
    FILE *f=fopen(argv[1],"rb");
    if(!f) { fprintf(stderr,"cannot open %s\n",argv[1]); return 2; }
    fseek(f,0,SEEK_END); long size=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *file=malloc((size_t)size);
    if(fread(file,1,(size_t)size,f)!=(size_t)size) { fprintf(stderr,"short read\n"); return 2; }
    fclose(f);

    // 1
    opus_pak_t k;
    const char *why=opus_pak_parse(file,(uint32_t)size,&k);
    CHECK(!why,"opus_pak_parse: %s",why?why:"");
    if(why) return 1;
    printf("%s: %u packets, %u frames, preSkip %u, index %u x %u, data at %u\n",
           argv[1],(unsigned)k.packet_count,(unsigned)k.total_frames,k.pre_skip,
           (unsigned)k.index_count,k.index_interval,(unsigned)k.data_offset);

    // 2 and 5: the straight walk.
    uint32_t at=k.data_offset, count=0;
    // Sized from the file rather than from the header, because one of the things
    // under test is whether the header's packet count is true.
    uint32_t *offset=malloc(sizeof *offset*((size_t)size/3+2));
    while(at+2<=(uint32_t)size) {
        uint32_t n=(uint32_t)file[at]|((uint32_t)file[at+1]<<8);
        CHECK(n>0&&at+2+n<=(uint32_t)size,"packet %u runs past the end",(unsigned)count);
        if(!n||at+2+n>(uint32_t)size) break;
        CHECK(n<=k.max_packet,"packet %u is %u bytes, header says max %u",
              (unsigned)count,(unsigned)n,k.max_packet);
        CHECK(opus_pak_toc_ok(file[at+2]),
              "packet %u is not CELT 20 ms mono (toc=0x%02x)",(unsigned)count,file[at+2]);
        offset[count]=at;
        at+=2+n; count++;
    }
    CHECK(count==k.packet_count,"walked %u packets, header says %u",
          (unsigned)count,(unsigned)k.packet_count);
    CHECK(at==(uint32_t)size,"the walk ended at %u of %ld",(unsigned)at,size);
    CHECK(k.total_frames<=count*OPUS_PAK_FRAME_SAMPLES&&
          k.total_frames+k.pre_skip+OPUS_PAK_FRAME_SAMPLES>count*OPUS_PAK_FRAME_SAMPLES,
          "totalFrames %u does not match %u packets with preSkip %u",
          (unsigned)k.total_frames,(unsigned)count,k.pre_skip);

    // 3: the same sequence through the slot walk the pump performs.
    uint32_t feed=k.data_offset, sliced=0, slots=0;
    while(feed<(uint32_t)size) {
        uint32_t want=SOUND_STREAM_SLOT_BYTES;
        if(want>(uint32_t)size-feed) want=(uint32_t)size-feed;
        uint32_t whole=opus_pak_whole(file+feed,want);
        CHECK(whole>0,"slot at %u trimmed to nothing",(unsigned)feed);
        if(!whole) break;
        CHECK(whole<=want,"slot at %u trimmed to more than it read",(unsigned)feed);
        // Every packet inside the published part is one the straight walk saw,
        // at the same file offset and the same length.
        uint32_t in=0;
        while(in<whole) {
            uint32_t n=(uint32_t)file[feed+in]|((uint32_t)file[feed+in+1]<<8);
            CHECK(sliced<count&&offset[sliced]==feed+in,
                  "sliced packet %u is at %u, the straight walk had it at %u",
                  (unsigned)sliced,(unsigned)(feed+in),
                  (unsigned)(sliced<count?offset[sliced]:0));
            in+=2+n; sliced++;
        }
        CHECK(in==whole,"a slot ended inside a packet");
        feed+=whole; slots++;
    }
    CHECK(sliced==count,"the slot walk found %u packets, the straight walk %u",
          (unsigned)sliced,(unsigned)count);
    printf("  %u slots of at most %u bytes carried all %u packets\n",
           (unsigned)slots,(unsigned)SOUND_STREAM_SLOT_BYTES,(unsigned)sliced);

    // 4: the index, which is the whole of seek().
    for(uint32_t i=0;i<k.index_count;i++) {
        uint32_t entry=(uint32_t)file[k.index_offset+i*4]|
                       ((uint32_t)file[k.index_offset+i*4+1]<<8)|
                       ((uint32_t)file[k.index_offset+i*4+2]<<16)|
                       ((uint32_t)file[k.index_offset+i*4+3]<<24);
        uint32_t packet=i*(uint32_t)k.index_interval;
        CHECK(packet<count,"index entry %u names packet %u of %u",
              (unsigned)i,(unsigned)packet,(unsigned)count);
        if(packet>=count) continue;
        CHECK(entry==offset[packet],
              "index entry %u says packet %u is at %u; it is at %u",
              (unsigned)i,(unsigned)packet,(unsigned)entry,(unsigned)offset[packet]);
    }
    printf("  %u index entries all land on packet boundaries\n",(unsigned)k.index_count);

    // The refusals, on a copy, so a wrong header is a refusal and not a walk.
    unsigned char h[OPUS_PAK_HEADER];
    opus_pak_t junk;
    memcpy(h,file,sizeof h);
    h[0]='X';
    CHECK(opus_pak_parse(h,sizeof h,&junk)!=NULL,"a bad magic was accepted");
    memcpy(h,file,sizeof h); h[12]=0xd0; h[13]=0x07;      // 2000 frames
    CHECK(opus_pak_parse(h,(uint32_t)size,&junk)!=NULL,"a 2000-sample frame was accepted");
    memcpy(h,file,sizeof h); h[14]=0xff; h[15]=0xff;      // maxPacket 65535
    CHECK(opus_pak_parse(h,(uint32_t)size,&junk)!=NULL,"an oversized packet was accepted");
    CHECK(opus_pak_parse(file,OPUS_PAK_HEADER-1,&junk)!=NULL,"a short file was accepted");

    // The gate, against the TOC space it is supposed to divide.
    // toc = config<<3 | stereo<<2 | code. The three axes the gate divides on,
    // one case either side of each.
    CHECK(!opus_pak_toc_ok(0x00),"SILK NB 10 ms passed the gate");       // config 0
    CHECK(!opus_pak_toc_ok(0x60),"hybrid SWB 10 ms passed the gate");    // config 12
    CHECK(!opus_pak_toc_ok(0xa8),"CELT WB 10 ms passed the gate");       // config 21
    CHECK(!opus_pak_toc_ok(0xbc),"CELT WB 20 ms STEREO passed the gate");// 23, stereo
    CHECK(!opus_pak_toc_ok(0xb9),"a two-frame packet passed the gate");  // 23, code 1
    CHECK(opus_pak_toc_ok(0x98),"CELT NB 20 ms was refused");            // config 19
    CHECK(opus_pak_toc_ok(0xb8),"CELT WB 20 ms was refused");            // config 23
    CHECK(opus_pak_toc_ok(0xd8),"CELT SWB 20 ms was refused");           // config 27
    CHECK(opus_pak_toc_ok(0xf8),"CELT FB 20 ms was refused");            // config 31

    free(offset); free(file);
    printf(fails?"%d FAILED\n":"all checks passed\n",fails);
    return fails?1:0;
}
