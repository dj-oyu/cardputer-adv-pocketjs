// Which rows does the menu occupy, and where may a decoration go?
//
//   gcc -O2 -Wall -Wextra -Werror tools/test_menu_rows.c -lm -o /tmp/mr && /tmp/mr
//
// This test exists because an assertion elsewhere was written from one
// screenshot: a decoration was placed at rows 112..132 and asserted to miss the
// menu, on the strength of a state in which the labels happened to sit at 37,
// 69 and 89. The board put SKK PRACTICE across 108..125. That decoration has
// since been removed for costing 3.3 ms; the rule it forced someone to write
// down is the part that was worth keeping.
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
    // The property any overlay relies on, checked where it lives: the overlay
    // hook must run BEFORE paint_labels in the strip loop, or a scene's
    // decoration draws over the interface instead of under it.
    //
    // FLOWER has no overlay today -- it had one, a trace of the swarm's births
    // and deaths, and that is what made someone write this file. It measured
    // 3.3 to 4.0 ms and came out. The other three backgrounds still have
    // overlays and still depend on this order, and the next thing that wants to
    // draw under the menu will depend on it too.
    //
    // It is a fact about the order of two calls in one function, so this reads
    // that function. Crude, and still an assertion about the real thing rather
    // than about a copy of it.
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
        printf("     overlays are drawn before paint_labels, which is what"
               " keeps text over a decoration rather than under\n");
    }
    return 0;
}
