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

/* The part of the box span can ink (ksn_ports.h): the advances of the scalars
 * the reveal lets through -- a cell inks only its first `width` <= `advance`
 * columns, tofu included -- and one line. The walk is span's own, so the two
 * count the same scalars. */
static void extent(void *ctx,const ksn_draw *draw,uint16_t reveal,unsigned *width,unsigned *height){
    (void)ctx;
    *width=0;*height=0;
    if(!draw||draw->kind!=KSN_TEXT||(unsigned)draw->data.text.font>KSN_DISPLAY)return;
    ksn_font font=draw->data.text.font;
    const char *s=draw->data.text.utf8;
    size_t bytes=draw->data.text.bytes;
    unsigned pen=0;
    for(size_t at=0;at<bytes&&reveal;reveal--){
        size_t consumed;
        uint32_t cp=utf8_decode(s,bytes,at,&consumed);at+=consumed;
        pen+=ksn_font_advance(font,cp);
    }
    *width=pen;
    *height=pen?(font==KSN_BODY?12u:font==KSN_DISPLAY?16u:8u):0u;
}

/* The ink of span_columns, as runs over a rectangle (ksn_ports.h). Each
 * scalar is decoded and looked up once for all the rectangle's rows; a cell
 * owns the columns [pen, pen+width), so the cells' runs never overlap. The
 * ink tests are span_columns' own, line for line. */
static void runs(void *ctx,const ksn_draw *draw,uint16_t reveal,int x0,int x1,int y0,int y1,
                 ksn_ink_run run,void *user){
    (void)ctx;
    if(!draw||draw->kind!=KSN_TEXT||(unsigned)draw->data.text.font>KSN_DISPLAY||!run)return;
    ksn_font font=draw->data.text.font;
    unsigned scale=font==KSN_DISPLAY?2:1;
    unsigned shift=font==KSN_DISPLAY?1u:0u;
    unsigned line_height=font==KSN_BODY?12:8*scale;
    const int top=draw->bounds.y0;
    if(y0<top)y0=top;
    if(y1>top+(int)line_height)y1=top+(int)line_height;
    if(y0>=y1||x0>=x1)return;
    int pen=draw->bounds.x0;
    const char *s=draw->data.text.utf8;
    size_t bytes=draw->data.text.bytes;
    for(size_t at=0;at<bytes&&reveal&&pen<x1;reveal--){
        size_t consumed;
        uint32_t cp=utf8_decode(s,bytes,at,&consumed);at+=consumed;
        jpfont_bitmap_view glyph={0};
        bool mapped=(font==KSN_BODY||cp>=128)&&
            jpfont_bitmap(font==KSN_BODY?JPFONT_TEXT:JPFONT_SMALL,cp,&glyph);
        unsigned advance=ksn_font_advance(font,cp);
        unsigned width=mapped?glyph.width*scale:advance;
        if(width>advance)width=advance;
        const int left=pen;
        pen+=(int)advance;
        int c0=left>x0?left:x0,c1=left+(int)width<x1?left+(int)width:x1;
        if(c0>=c1)continue;
        unsigned ch=cp>=32&&cp<=126?cp:'?';
        /* The cell's columns in the rectangle, as bits of gx (bit i: gx=i):
         * a cell is at most 24 columns, so one row of it is one word. */
        const uint32_t window=(uint32_t)((((uint64_t)1<<(c1-left))-1u)&~(((uint64_t)1<<(c0-left))-1u));
        for(int y=y0;y<y1;y++){
            const unsigned gy=(unsigned)(y-top),row=gy>>shift;
            uint32_t ink=0;
            if(mapped){
                if(row>=glyph.height)continue;
                /* The row's cells, MSB first, then each glyph column c widened
                 * to its `scale` pixels gx = c<<shift... */
                const uint8_t *bits=glyph.bits+row*glyph.stride;
                const unsigned cols=(width+scale-1)>>shift;   /* glyph columns that reach gx < width */
                for(unsigned c=0;c<cols;c++)
                    if(bits[c>>3]&(0x80u>>(c&7)))ink|=(scale==2?3u:1u)<<(c<<shift);
            }else if(cp<128){
                if(row>=7)continue;
                const unsigned r=font_rows[(ch-32)*7+row];
                for(unsigned c=0;c<5;c++)
                    if(r&(1u<<(4-c)))ink|=(scale==2?3u:1u)<<(c<<shift);
            }else{
                /* Tofu: the cell's outline, gx < advance-1 and gy < line-1. */
                if((int)gy>=(int)line_height-1||advance<2)continue;
                const uint32_t inner=(uint32_t)((((uint64_t)1<<(advance-1))-1u));
                ink=gy==0||(int)gy==(int)line_height-2?inner:(1u|(1u<<(advance-2)))&inner;
            }
            /* ...and only the columns in the rectangle and under the cell's width. */
            ink&=window&(uint32_t)((((uint64_t)1<<width)-1u));
            while(ink){
                const unsigned at=(unsigned)__builtin_ctz(ink);
                const unsigned n=(unsigned)__builtin_ctz(~(ink>>at));
                run(user,left+(int)at,y,n);
                ink&=~(uint32_t)((((uint64_t)1<<n)-1u)<<at);
            }
        }
    }
}

const ksn_text_port ksn_font_port={.span=span,.advance=advance_of,.binary_coverage=true,.extent=extent,
                                   .runs=runs};
