// Which rows does the menu occupy, and where may a decoration go?
//
//   gcc -O2 -Wall -Wextra -Werror tools/test_menu_rows.c -lm -o /tmp/mr && /tmp/mr
//
// This test exists because an assertion in tools/test_garden.c was written from
// one screenshot: the garden's trace was placed at rows 112..132 and asserted to
// miss the menu, on the strength of a state in which the labels happened to sit
// at 37, 69 and 89. The board put SKK PRACTICE across 108..125.
//
// The failure was not the measurement, it was the generalisation -- the same
// shape as a centroid bound fitted to a sixteen-mote swarm. So the answer is not
// a better screenshot. It is to derive the rows from the layout rule, over every
// state the machine rests in, and to state plainly that during a scroll no row
// is safe at all.
#include "../main/ui/menu_rows.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/scene/garden.h"
#define H 135
int main(void) {
    unsigned char rest[H]={0};
    // The resting states. The animated positions settle on integers, so the
    // deltas are integers; k is swept far enough past the screen in both
    // directions that no reachable row is missed for any menu length.
    for(int k=-8;k<=8;k++) {
        int y=(int)(menu_item_y((float)k)+0.5f);
        for(int r=y;r<y+MENU_TEXT_ROWS(MENU_ITEM_SCALE);r++)
            if(r>=0&&r<H)rest[r]=1;
    }
    for(int r=MENU_CATEGORY_Y;r<MENU_CATEGORY_Y+MENU_TEXT_ROWS(1);r++)rest[r]=1;
    for(int r=MENU_DETAIL_Y;r<MENU_DETAIL_Y+MENU_TEXT_ROWS(1);r++)rest[r]=1;
    printf("MENU rows occupied at rest:");
    for(int r=0;r<H;r++)
        if(rest[r]&&(r==0||!rest[r-1])) {
            int e=r;while(e<H&&rest[e])e++;
            printf(" %d..%d",r,e-1);
        }
    printf("\n     free bands:");
    int best=0,best_lo=0;
    for(int r=0;r<H;r++)
        if(!rest[r]&&(r==0||rest[r-1])) {
            int e=r;while(e<H&&!rest[e])e++;
            printf(" %d..%d",r,e-1);
            if(e-r>best){best=e-r;best_lo=r;}
        }
    printf("\n     widest free band %d..%d (%d rows)\n",best_lo,best_lo+best-1,best);
#if GARDEN_ECG
    // The trace has to fit a band the menu does not rest on. This is the
    // assertion the screenshot version could not make, and it is deliberately
    // stated over the whole box the trace can reach -- baseline, waver and the
    // tallest spike together -- rather than over the rows it happened to use in
    // one frame.
    int lo=GARDEN_ECG_Y-GARDEN_ECG_WAVER-GARDEN_ECG_SPIKE;
    int hi=GARDEN_ECG_Y+GARDEN_ECG_WAVER+GARDEN_ECG_SPIKE+GARDEN_ECG_THICK-1;
    assert(lo>=0&&hi<H);
    int shared=0;
    for(int r=lo;r<=hi;r++)if(rest[r])shared++;
    // NOT disjointness. The trace used to have to miss the menu's resting rows,
    // and that assertion was right for an eleven-row ornament sitting in a gap.
    // It is the wrong property now, and it was always the weaker argument: what
    // keeps the two apart is the DRAW ORDER, not the geometry, and the order
    // does not care whether the band is eleven rows or thirty-nine. Asserting
    // disjointness for a taller trace would only have forced it back to eleven.
    //
    // Reported rather than bounded tightly, because the number is the thing to
    // watch: if a later change puts most of the trace under the menu's own
    // resting row it should show up here and not on the glass.
    assert(shared*2<hi-lo+1);
    printf("ECG_ROWS_OK: the trace's box is %d..%d (%d rows), of which %d are"
           " rows the menu rests on\n",lo,hi,hi-lo+1,shared);
    // While the menu scrolls, item_y sweeps continuously and every row is
    // crossed; that is a fact about the layout, not a bound, and it is why the
    // claim is "where the menu does not rest" and never "where it never is".
    {
        int crossed=0;
        for(int i=0;i<=200;i++) {
            float d=-4.0f+i*8.0f/200;
            int y=(int)(menu_item_y(d)+0.5f);
            if(y+MENU_TEXT_ROWS(MENU_ITEM_SCALE)>lo&&y<=hi)crossed=1;
        }
        assert(crossed);        /* if this ever stops being true, say so */
    }
    // The property the trace actually relies on, checked where it lives: the
    // overlay hook must run BEFORE paint_labels in the strip loop, or text and
    // trace swap places and an ornament draws over the interface. It is a fact
    // about the order of two calls in one function, so this reads that
    // function. Crude, and still an assertion about the real thing rather than
    // about a copy of it.
    {
        FILE *fp=fopen("main/ui/shell.c","rb");
        assert(fp);
        static char src[262144];
        size_t n=fread(src,1,sizeof src-1,fp);
        fclose(fp);
        src[n]=0;
        const char *ovl=strstr(src,"sc->overlay(strip,strip_y,strip_h)");
        const char *lab=strstr(src,"paint_labels()");
        assert(ovl&&lab&&ovl<lab);
        printf("             drawn before paint_labels, which is the whole of"
               " what keeps the text over it rather than under\n");
    }
#endif
    return 0;
}
