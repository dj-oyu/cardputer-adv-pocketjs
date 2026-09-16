// Host stand-in for scene/canopy_pie.c, for harnesses that #include garden.c
// without linking the kernel: garden_row calls canopy_pie, and the decor tests
// measure the decor loop, so the canopy is a no-op and both arms of a decor A/B
// see the same canopy. The signature is scene/canopy_pie.h's, verbatim.
#include <stdint.h>
void canopy_pie(uint16_t *row,int n,int cx,int mrr,int qy,uint16_t leafy,int x0) {
    (void)row;(void)n;(void)cx;(void)mrr;(void)qy;(void)leafy;(void)x0;
}
