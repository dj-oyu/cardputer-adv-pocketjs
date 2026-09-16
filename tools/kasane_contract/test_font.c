#include <assert.h>
#include <stdio.h>
#include "ksn_font.h"
/* Exercise the actual mapped-cell reader with deterministic, bounded images. */
#include "../../main/text/jpfont.c"
static union { uint32_t align; uint8_t bytes[512]; } image;
static void setup(void){
    jpf_hdr_t header={.magic=JPF_MAGIC,.version=1,.cell_w=12,.cell_h=12,
        .count=3,.cmap_off=sizeof(jpf_hdr_t),.bitmap_off=64,.bitmap_len=72};
    jpf_cmap_t cmap[3]={{0,12,0},{'A',6,0},{0x3042,12,0}};
    memcpy(image.bytes,&header,sizeof(header));memcpy(image.bytes+header.cmap_off,cmap,sizeof(cmap));
    for(unsigned glyph=0;glyph<3;glyph++)for(unsigned row=0;row<12;row++){
        image.bytes[64+glyph*24+row*2]=(uint8_t)(0x80u>>(row%6));
        image.bytes[65+glyph*24+row*2]=(uint8_t)(glyph==2?0xa0:0);
    }
    assert(open_face(&faces[JPFONT_TEXT],image.bytes,sizeof(image.bytes)));
}
static void read_span(ksn_draw *d,unsigned reveal,int x,int y,unsigned count,uint8_t *out){
    assert(ksn_font_port.span(NULL,d,(uint16_t)reveal,x,y,count,out)==KSN_OK);
}
int main(void){
    setup();
    jpfont_bitmap_view bitmap;
    assert(!jpfont_bitmap(JPFONT_COUNT,0x3042,&bitmap));
    assert(!jpfont_bitmap(JPFONT_TEXT,0x3042,NULL));
    assert(jpfont_bitmap(JPFONT_TEXT,0x3042,&bitmap));
    assert(bitmap.bits==image.bytes+112&&bitmap.width==12&&bitmap.height==12&&bitmap.stride==2);
    uint8_t expanded[144],actual[64];
    jpfont_glyph(JPFONT_TEXT,0x3042,expanded);
    ksn_draw d={.kind=KSN_TEXT,.bounds={-3,7,200,30},.clip={0,0,240,135},.opacity=255,
        .data.text={.utf8="\xe3\x81\x82" "A",.bytes=4,.font=KSN_BODY}};
    /* Non-NUL counted UTF-8, negative origin, every row across strip boundaries. */
    for(int row=0;row<12;row++){
        read_span(&d,1,-3,row+7,24,actual);
        assert(!memcmp(actual,expanded+row*12,12));
        for(int x=12;x<24;x++)assert(actual[x]==0);
        uint8_t clipped[7];read_span(&d,1,2,row+7,7,clipped);
        assert(!memcmp(clipped,expanded+row*12+5,7));
    }
    read_span(&d,0,-3,7,24,actual);for(int i=0;i<24;i++)assert(actual[i]==0);
    read_span(&d,2,-3,7,24,actual);assert(actual[12]==255);
    /* Four-byte scalars consume one reveal position and preserve the next pen. */
    d.data.text.utf8="\xf0\x9f\x90\x88" "A";d.data.text.bytes=5;
    read_span(&d,1,-3,7,24,actual);assert(actual[0]==255&&actual[12]==0);
    read_span(&d,2,-3,7,24,actual);assert(actual[12]==255);
    /* Builtin A: row 0 is 01110, and display is exact nearest-neighbor 2x. */
    d.data.text.utf8="AA";d.data.text.bytes=2;d.data.text.font=KSN_CAPTION;
    read_span(&d,2,-3,7,12,actual);
    const uint8_t a[6]={0,255,255,255,0,0};assert(!memcmp(actual,a,6)&&!memcmp(actual+6,a,6));
    d.data.text.font=KSN_DISPLAY;read_span(&d,2,-3,8,24,actual);
    for(int i=0;i<24;i++)assert(actual[i]==a[(i/2)%6]);
    /* Chunks that start inside a cell, and the reveal count that decides
     * whether that cell is reached at all: cells left of the chunk own none of
     * its columns and still consume one reveal position each. The origin is 0
     * here so the cells sit at 0, 6 and 12. */
    d.data.text.font=KSN_CAPTION;d.data.text.utf8="AAA";d.data.text.bytes=3;
    d.bounds.x0=0;
    read_span(&d,1,7,7,4,actual);                       /* cell 1 is not reached */
    for(int i=0;i<4;i++)assert(actual[i]==0);
    read_span(&d,2,7,7,4,actual);                       /* cell 1, columns 1..4 */
    assert(actual[0]==255&&actual[1]==255&&actual[2]==255&&actual[3]==0);
    read_span(&d,3,11,7,4,actual);                      /* cell 2, columns 0..2 */
    assert(actual[0]==0&&actual[1]==0&&actual[2]==255&&actual[3]==255);
    d.bounds.x0=-3;
    /* Missing Japanese face: visible tofu with the documented 8-pixel advance. */
    d.data.text.font=KSN_CAPTION;d.data.text.utf8="\xe3\x81\x82" "A";d.data.text.bytes=4;
    read_span(&d,2,-3,7,16,actual);assert(actual[0]==255&&actual[6]==255&&actual[7]==0);
    assert(!memcmp(actual+8,a,6));
    /* No face is also valid for BODY, without changing Latin/fullwidth metrics. */
    memset(faces,0,sizeof(faces));d.data.text.font=KSN_BODY;
    read_span(&d,2,-3,7,24,actual);assert(actual[10]==255&&actual[11]==0);
    assert(!memcmp(actual+12,a,6));
    assert(ksn_font_port.span(NULL,&d,2,0,0,0,NULL)==KSN_OK);
    assert(ksn_font_port.span(NULL,&d,2,0,0,65,actual)==KSN_INVALID);
    puts("font PASS: mapped 1bpp/UTF-8/reveal, negative clipping, caption/body/display, fallback metrics");
}
