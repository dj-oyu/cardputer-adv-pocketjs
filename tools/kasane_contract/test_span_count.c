#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ksn_font.h"
/* The host face reader: no face is opened below, so ASCII takes the builtin 5x7
 * rows -- the same configuration the counts were measured with. */
#include "../../main/text/jpfont.c"

/* The ink loop's contract, both arms. Built with -DKSN_SPAN_COUNT (run.sh), so
 * main/text/ksn_font.c publishes how many cells it decoded and how many chunk
 * columns the ink loop visited.
 *
 * The four texts and origins are apps/hello/main.js's, split into the
 * 64-column chunks one repaint asks for. `narrow` is what the shipped arm
 * (g_ksn_span_narrow = 1) walks; `chunk` is the control arm, which walks every
 * column of the chunk for every cell -- 22,912 columns for the counter line
 * alone, 70 instructions each with 4 of them a division. Pinning both numbers
 * keeps the control comparable: it must stay the pre-change walk, or the
 * device A/B measures two changes at once.
 *
 * Pixels are not the subject here -- tools/kasane_contract/test_font.c pins the
 * masks, and the exhaustive arm-against-arm comparison (47,900,160 calls) is in
 * the commit that added the narrow walk. */
extern uint32_t ksn_span_cells,ksn_span_columns;

typedef struct {
    const char *name;const char *text;size_t bytes;ksn_font font;ksn_rect bounds;
    uint32_t narrow_columns;uint32_t chunk_columns;uint32_t cells;
} hello_line;

static const hello_line lines[]={
    {"caption","KASANE / JAVASCRIPT",19,KSN_CAPTION,{16,12,226,22},      912,27824,544},
    {"display","Hello, World!",13,KSN_DISPLAY,{16,34,236,52},          2496,36544,688},
    {"counter","KEY PRESSES: 1234",18,KSN_CAPTION,{28,77,212,89},       864,22912,376},
    {"hints","ENTER +1    ESC HOME",20,KSN_CAPTION,{16,117,236,127},    960,30592,568},
};

static void repaint(const hello_line *line,uint32_t *columns,uint32_t *cells){
    unsigned scale=line->font==KSN_DISPLAY?2:1;
    unsigned line_height=line->font==KSN_BODY?12:8*scale;
    uint32_t columns_before=ksn_span_columns,cells_before=ksn_span_cells;
    unsigned spans=0;
    for(unsigned gy=0;gy<line_height;gy++)
        for(int x=line->bounds.x0;x<line->bounds.x1;){
            unsigned count=(unsigned)(line->bounds.x1-x);
            if(count>64)count=64;
            ksn_draw d={.kind=KSN_TEXT,.bounds=line->bounds,.clip={0,0,240,135},
                .opacity=255,.data.text={.utf8=line->text,.bytes=line->bytes,
                .font=line->font}};
            uint8_t out[64];
            assert(ksn_font_port.span(NULL,&d,255,x,line->bounds.y0+(int)gy,count,out)==KSN_OK);
            x+=(int)count;spans++;
        }
    *columns=ksn_span_columns-columns_before;
    *cells=ksn_span_cells-cells_before;
    printf("  %-8s spans=%3u\n",line->name,spans);
}

int main(void){
    uint32_t columns,cells;
    for(unsigned i=0;i<sizeof(lines)/sizeof(lines[0]);i++){
        const hello_line *line=&lines[i];
        assert(g_ksn_span_narrow==1);                   /* the shipped arm */
        repaint(line,&columns,&cells);
        printf("    narrow  columns=%5u cells=%u\n",columns,cells);
        assert(columns==line->narrow_columns);
        assert(cells==line->cells);
        g_ksn_span_narrow=0;                            /* the control arm */
        repaint(line,&columns,&cells);
        printf("    chunk   columns=%5u cells=%u\n",columns,cells);
        assert(columns==line->chunk_columns);
        assert(cells==line->cells);                     /* the same walk, wider */
        g_ksn_span_narrow=1;
    }
    /* A chunk far to the right of a long line walks only the cells that reach
     * it, and a cell that owns none of the chunk's columns walks none. */
    uint32_t before=ksn_span_columns;
    ksn_draw d={.kind=KSN_TEXT,.bounds={0,0,240,8},.clip={0,0,240,135},.opacity=255,
        .data.text={.utf8="iiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiiii",.bytes=36,.font=KSN_CAPTION}};
    uint8_t out[64];
    assert(ksn_font_port.span(NULL,&d,255,192,0,48,out)==KSN_OK);
    assert(ksn_span_columns-before<=48);
    puts("span count PASS: narrow walks the cells' columns, the control walks the chunk");
}
