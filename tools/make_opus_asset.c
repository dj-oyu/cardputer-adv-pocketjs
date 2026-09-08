// Encode a 24 kHz mono PCM16 WAV into the container main/pocket/opus_feed.h
// describes, for embedding in assets:/ or dropping on a card.
//
//     bash tools/make_opus_asset.sh IN.wav OUT.pok [bitrate]
//
// The precedent is tools/make_wav_asset.py, and the division of labour with it
// is deliberate: that script owns resampling and the anti-alias filter (44.1 kHz
// stereo down to this host's 24 kHz mono), and this program owns only the
// encode. Feed it that script's output.
//
// It is C rather than Python because the encoder is libopus and there is no
// binding in this tree; it links the same .cache/codecs build the study used.
//
// WHY RESTRICTED_LOWDELAY. It forces CELT for every frame. The firmware refuses
// SILK and hybrid packets by name, because the decode stack it sizes for
// (10,420 bytes) was measured for CELT 20 ms mono and is only a lower bound for
// the others -- see the gate in opus_feed.h. An encoder left on
// OPUS_APPLICATION_AUDIO would choose SILK for some content and produce a file
// this host refuses at open(), which is a worse failure than a slightly larger
// one. This program checks the TOC byte of every packet it writes with the same
// predicate the firmware uses, so an asset that would be refused on the device
// is refused here instead.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <opus.h>

#define RATE   24000
#define FRAME  480            // 20 ms, the only packet length the host decodes
#define INDEX_INTERVAL 5      // one seek entry per 100 ms

static unsigned char *g_out;
static size_t g_len, g_cap;
static void put(const void *p, size_t n) {
    if(g_len+n>g_cap) { g_cap=(g_len+n)*2+4096; g_out=realloc(g_out,g_cap); }
    memcpy(g_out+g_len,p,n); g_len+=n;
}
static void put16(unsigned v){ unsigned char b[2]={v&255,(v>>8)&255}; put(b,2); }
static void put32(unsigned long v){
    unsigned char b[4]={v&255,(v>>8)&255,(v>>16)&255,(v>>24)&255}; put(b,4); }

// The same predicate as opus_pak_toc_ok() in main/pocket/opus_feed.h. Duplicated
// rather than shared because this program builds on a host against upstream
// headers and that one builds into the firmware; the cost of the duplication is
// four lines and the benefit is that the generator has no include path into
// main/. tools/test_opus_pak.c compiles the firmware's copy and checks the file
// this one wrote, which is where the two are actually reconciled.
static int toc_ok(unsigned char toc) {
    unsigned config=toc>>3, stereo=(toc>>2)&1, code=toc&3;
    return config>=16 && (config&3)==3 && !stereo && code==0;
}

// Enough of a WAV reader for what make_wav_asset.py writes, and no more: it
// refuses anything that is not 24 kHz mono PCM16 rather than converting it,
// because converting is the other tool's job and doing it in two places is how
// two tools come to disagree.
static short *read_wav(const char *path, unsigned long *count) {
    FILE *f=fopen(path,"rb");
    if(!f) { fprintf(stderr,"cannot open %s\n",path); return NULL; }
    unsigned char h[12];
    if(fread(h,1,12,f)!=12||memcmp(h,"RIFF",4)||memcmp(h+8,"WAVE",4)) {
        fprintf(stderr,"%s is not a WAV file\n",path); fclose(f); return NULL; }
    short *pcm=NULL; unsigned long n=0; int have_fmt=0;
    for(;;) {
        unsigned char c[8];
        if(fread(c,1,8,f)!=8) break;
        unsigned long body=c[4]|(c[5]<<8)|((unsigned long)c[6]<<16)|((unsigned long)c[7]<<24);
        if(!memcmp(c,"fmt ",4)) {
            unsigned char b[16];
            if(body<16||fread(b,1,16,f)!=16) break;
            unsigned fmt=b[0]|(b[1]<<8), ch=b[2]|(b[3]<<8), bits=b[14]|(b[15]<<8);
            unsigned long rate=b[4]|(b[5]<<8)|((unsigned long)b[6]<<16)|((unsigned long)b[7]<<24);
            if(fmt!=1||ch!=1||bits!=16||rate!=RATE) {
                fprintf(stderr,"%s must be 24000 Hz mono PCM16 "
                               "(got format %u, %u ch, %u bit, %lu Hz). "
                               "Run it through tools/make_wav_asset.py first.\n",
                        path,fmt,ch,bits,rate);
                fclose(f); return NULL;
            }
            have_fmt=1;
            if(body>16) fseek(f,(long)(body-16),SEEK_CUR);
        } else if(!memcmp(c,"data",4)) {
            n=body/2;
            pcm=malloc(n*sizeof *pcm);
            if(fread(pcm,2,n,f)!=n) { free(pcm); pcm=NULL; break; }
        } else fseek(f,(long)(body+(body&1)),SEEK_CUR);
    }
    fclose(f);
    if(!have_fmt||!pcm) { fprintf(stderr,"%s has no usable audio\n",path); free(pcm); return NULL; }
    *count=n;
    return pcm;
}

int main(int argc, char **argv) {
    if(argc<3) { fprintf(stderr,"usage: %s IN.wav OUT.pok [bitrate]\n",argv[0]); return 2; }
    long bitrate=argc>3?strtol(argv[3],NULL,10):24000;

    unsigned long samples=0;
    short *pcm=read_wav(argv[1],&samples);
    if(!pcm) return 1;

    int err=0;
    OpusEncoder *enc=opus_encoder_create(RATE,1,OPUS_APPLICATION_RESTRICTED_LOWDELAY,&err);
    if(!enc||err!=OPUS_OK) { fprintf(stderr,"opus_encoder_create: %d\n",err); return 1; }
    opus_encoder_ctl(enc,OPUS_SET_BITRATE((opus_int32)bitrate));
    opus_encoder_ctl(enc,OPUS_SET_VBR(1));
    opus_encoder_ctl(enc,OPUS_SET_FORCE_CHANNELS(1));
    opus_int32 lookahead=0;
    opus_encoder_ctl(enc,OPUS_GET_LOOKAHEAD(&lookahead));

    // The encoder's own delay, appended as silence so the last real sample still
    // comes out the other side, and recorded in the header so the decoder drops
    // exactly as many as were added at the front. Getting this wrong is not a
    // crash, it is a clip that starts a few milliseconds late for ever.
    unsigned long total=samples+(unsigned long)lookahead;
    unsigned long packets=(total+FRAME-1)/FRAME;
    unsigned long index_count=(packets+INDEX_INTERVAL-1)/INDEX_INTERVAL;
    unsigned long data_offset=36+index_count*4;

    unsigned char *body=NULL; size_t body_len=0, body_cap=0;
    unsigned long *index=calloc(index_count,sizeof *index);
    unsigned max_packet=0;
    unsigned char pkt[1500];
    short frame[FRAME];
    for(unsigned long p=0;p<packets;p++) {
        for(int i=0;i<FRAME;i++) {
            unsigned long at=p*FRAME+(unsigned long)i;
            frame[i]=at<samples?pcm[at]:0;
        }
        int n=opus_encode(enc,frame,FRAME,pkt,(opus_int32)sizeof pkt);
        if(n<1) { fprintf(stderr,"opus_encode failed at packet %lu: %d\n",p,n); return 1; }
        if(!toc_ok(pkt[0])) {
            fprintf(stderr,"packet %lu is not CELT 20 ms mono (toc=0x%02x); "
                           "the firmware would refuse this file\n",p,pkt[0]);
            return 1;
        }
        if((unsigned)n>max_packet) max_packet=(unsigned)n;
        if(p%INDEX_INTERVAL==0) index[p/INDEX_INTERVAL]=data_offset+body_len;
        if(body_len+2+(size_t)n>body_cap) {
            body_cap=(body_len+2+(size_t)n)*2+4096; body=realloc(body,body_cap); }
        body[body_len++]=(unsigned char)(n&255);
        body[body_len++]=(unsigned char)((n>>8)&255);
        memcpy(body+body_len,pkt,(size_t)n); body_len+=(size_t)n;
    }
    opus_encoder_destroy(enc);

    if(max_packet>2048-2) {
        fprintf(stderr,"a packet reached %u bytes, past what a 2,048-byte stream "
                       "slot holds; lower the bitrate\n",max_packet);
        return 1;
    }

    put("POK1",4);
    put16(1);                       // version
    put16(1);                       // channels
    put32(RATE);
    put16(FRAME);
    put16(max_packet);
    put32(packets);
    put32(samples);                 // totalFrames, lookahead already excluded
    put16((unsigned)lookahead);
    put16(INDEX_INTERVAL);
    put32(index_count);
    put32(data_offset);
    for(unsigned long i=0;i<index_count;i++) put32(index[i]);
    if(g_len!=data_offset) { fprintf(stderr,"header is %zu, not %lu\n",g_len,data_offset); return 1; }
    put(body,body_len);

    FILE *o=fopen(argv[2],"wb");
    if(!o||fwrite(g_out,1,g_len,o)!=g_len) { fprintf(stderr,"cannot write %s\n",argv[2]); return 1; }
    fclose(o);

    double seconds=(double)samples/RATE;
    fprintf(stderr,"%s: %lu packets, %.2f s, %zu bytes (%.0f B/s, %.1f kbit/s), "
                   "max packet %u, index %lu entries (%lu B), preSkip %ld\n",
            argv[2],packets,seconds,g_len,g_len/seconds,g_len*8/seconds/1000.0,
            max_packet,index_count,index_count*4,(long)lookahead);
    fprintf(stderr,"app:/'s 24,576-byte quota would hold %.2f s of this.\n",
            24576.0/(g_len/seconds));
    free(pcm); free(body); free(index); free(g_out);
    return 0;
}
