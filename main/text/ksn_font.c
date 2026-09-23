#include "ksn_font.h"
#include "jpfont.h"
#include "utf8.h"
#include "fonts.h"
#include <string.h>

#ifdef KSN_SPAN_COUNT
/* Host-side contract counters, the KSN_TILE_COUNT idiom: how many glyph cells
 * the walk decoded and how many chunk columns the ink loop actually visited.
 * The column count is the thing this kernel exists to shrink; what it is worth
 * on the board is a device A/B, not a host number. */
uint32_t ksn_span_cells;
uint32_t ksn_span_columns;
#endif

/* Same-binary A/B arm (main/app_session.c's switches[]): 1 is the shipped walk,
 * which gives a cell only the columns it owns. 0 is the pre-change walk, which
 * gave every cell every column of the chunk -- kept in the binary because a
 * build-to-build comparison cannot judge this: placement moves the same code by
 * up to 15% (docs/perf/pie-simd.md 6.3), and this change is a few tens of
 * percent of one bracket. The two arms write the same pixels (test_font.c
 * compares masks; the exhaustive comparison is in the commit that added this). */
int g_ksn_span_narrow=1;

/* The shipped arm. `scale` is 1 or 2, so the division in the ink test is a
 * shift, and the cell loop walks the chunk's columns rather than all 64 of
 * them: a cell is `advance` wide (6..24) and only its first `width` columns can
 * ever ink, so 6 or 12 columns of work out of 64 used to be paid, with two
 * `quou` (16..18 cycles each, docs/perf/pie-simd.md 233) per column. On the
 * board this function is the largest thing left in a patch frame: its bracket
 * is 48% of render_ms (40 calls, 6.2 kcycles each; KASANE_PAINT prof, one
 * binary, tools/host_kasane_opt_ab.py). */
static ksn_result span_columns(void *ctx,const ksn_draw *draw,uint16_t reveal,int x,int y,
                               unsigned count,uint8_t *out){
    (void)ctx;
    if(!draw||draw->kind!=KSN_TEXT||(unsigned)draw->data.text.font>KSN_DISPLAY||count>64||
       (count&&!out))return KSN_INVALID;
    if(!count)return KSN_OK;
    memset(out,0,count);
    ksn_font font=draw->data.text.font;
    unsigned scale=font==KSN_DISPLAY?2:1;
    unsigned shift=font==KSN_DISPLAY?1u:0u;
    unsigned line_height=font==KSN_BODY?12:8*scale;
    int gy=y-draw->bounds.y0;
    if(gy<0||gy>=(int)line_height)return KSN_OK;
    int pen=draw->bounds.x0;
    const char *s=draw->data.text.utf8;
    size_t bytes=draw->data.text.bytes;
    for(size_t at=0;at<bytes&&reveal&&pen<x+(int)count;reveal--){
        size_t consumed;
        uint32_t cp=utf8_decode(s,bytes,at,&consumed);at+=consumed;
        jpfont_bitmap_view glyph={0};
        bool mapped=(font==KSN_BODY||cp>=128)&&
            jpfont_bitmap(font==KSN_BODY?JPFONT_TEXT:JPFONT_SMALL,cp,&glyph);
        // Logical metrics do not change when a glyph/face is missing.
        unsigned advance=ksn_font_advance(font,cp);
        unsigned width=mapped?glyph.width*scale:advance;
        if(width>advance)width=advance;
#ifdef KSN_SPAN_COUNT
        ksn_span_cells++;
#endif
        /* The columns of this cell that land in the chunk: `i - left` is gx,
         * so the loop runs over [left, left+width) clipped to [0, count). A
         * cell wholly left of the chunk (left+width <= 0) or wholly right
         * (left >= count) runs zero times; it still consumes its reveal
         * position and advances the pen, which is why the walk cannot skip it.
         * Inside this range gx and gy are both non-negative, so the ink tests
         * take the shift form of their division. */
        int left=pen-x,right=left+(int)width;
        unsigned first=left<0?0u:(unsigned)left;
        unsigned end=right<0?0u:((unsigned)right>count?count:(unsigned)right);
#ifdef KSN_SPAN_COUNT
        if(end>first)ksn_span_columns+=end-first;
#endif
        for(unsigned i=first;i<end;i++){
            int gx=(int)i-left;
            bool ink=false;
            if(mapped){
                unsigned row=(unsigned)gy>>shift,col=(unsigned)gx>>shift;
                if(row<glyph.height)ink=(glyph.bits[row*glyph.stride+col/8]&(0x80u>>(col&7)))!=0;
            }else if(cp<128){
                unsigned row=(unsigned)gy>>shift,col=(unsigned)gx>>shift;
                if(row<7&&col<5){unsigned ch=cp>=32&&cp<=126?cp:'?';
                    ink=(font_rows[(ch-32)*7+row]&(1u<<(4-col)))!=0;}
            }else{
                // Missing face: a same-advance tofu, not an invisible glyph.
                ink=gx<(int)advance-1&&gy<(int)line_height-1&&
                    (gx==0||gx==(int)advance-2||gy==0||gy==(int)line_height-2);
            }
            if(ink)out[i]=255;
        }
        pen+=(int)advance;
    }
    return KSN_OK;
}

/* The control arm: the same walk as before the change. Every cell walks all
 * `count` columns and each one re-tests whether the cell owns it. Nothing else
 * differs -- same metrics, same reveal accounting, same `out` handling. */
static ksn_result span_chunk(void *ctx,const ksn_draw *draw,uint16_t reveal,int x,int y,
                             unsigned count,uint8_t *out){
    (void)ctx;
    if(!draw||draw->kind!=KSN_TEXT||(unsigned)draw->data.text.font>KSN_DISPLAY||count>64||
       (count&&!out))return KSN_INVALID;
    if(!count)return KSN_OK;
    memset(out,0,count);
    ksn_font font=draw->data.text.font;
    unsigned scale=font==KSN_DISPLAY?2:1;
    unsigned line_height=font==KSN_BODY?12:8*scale;
    int gy=y-draw->bounds.y0;
    if(gy<0||gy>=(int)line_height)return KSN_OK;
    int pen=draw->bounds.x0;
    const char *s=draw->data.text.utf8;
    size_t bytes=draw->data.text.bytes;
    for(size_t at=0;at<bytes&&reveal&&pen<x+(int)count;reveal--){
        size_t consumed;
        uint32_t cp=utf8_decode(s,bytes,at,&consumed);at+=consumed;
        jpfont_bitmap_view glyph={0};
        bool mapped=(font==KSN_BODY||cp>=128)&&
            jpfont_bitmap(font==KSN_BODY?JPFONT_TEXT:JPFONT_SMALL,cp,&glyph);
        unsigned advance=ksn_font_advance(font,cp);
        unsigned width=mapped?glyph.width*scale:advance;
        if(width>advance)width=advance;
#ifdef KSN_SPAN_COUNT
        ksn_span_cells++;
        ksn_span_columns+=count;
#endif
        for(unsigned i=0;i<count;i++){
            int gx=x+(int)i-pen;
            if(gx<0||gx>=(int)width)continue;
            bool ink=false;
            if(mapped){
                unsigned row=(unsigned)gy/scale,col=(unsigned)gx/scale;
                if(row<glyph.height)ink=(glyph.bits[row*glyph.stride+col/8]&(0x80u>>(col&7)))!=0;
            }else if(cp<128){
                unsigned row=(unsigned)gy/scale,col=(unsigned)gx/scale;
                if(row<7&&col<5){unsigned ch=cp>=32&&cp<=126?cp:'?';
                    ink=(font_rows[(ch-32)*7+row]&(1u<<(4-col)))!=0;}
            }else{
                ink=gx<(int)advance-1&&gy<(int)line_height-1&&
                    (gx==0||gx==(int)advance-2||gy==0||gy==(int)line_height-2);
            }
            if(ink)out[i]=255;
        }
        pen+=(int)advance;
    }
    return KSN_OK;
}

static ksn_result span(void *ctx,const ksn_draw *draw,uint16_t reveal,int x,int y,
                       unsigned count,uint8_t *out){
    if(g_ksn_span_narrow)return span_columns(ctx,draw,reveal,x,y,count,out);
    return span_chunk(ctx,draw,reveal,x,y,count,out);
}

/* The same expression both span arms use for the pen step, and deliberately the
 * only copy of it that is not inside them: it is the LOGICAL advance, so it
 * never consults jpfont -- a missing face draws tofu of this width rather than
 * moving the pen (the "Logical metrics do not change" comment above). Damage
 * uses it to turn "scalars 13 through 14 differ" into a column range. */
static unsigned advance_of(void *ctx,ksn_font font,uint32_t codepoint){
    (void)ctx;
    if((unsigned)font>KSN_DISPLAY)return 0;
    return ksn_font_advance(font,codepoint);
}

const ksn_text_port ksn_font_port={.span=span,.advance=advance_of};
