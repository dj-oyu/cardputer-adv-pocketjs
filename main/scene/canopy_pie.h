// The canopy kernel's entry point. The kernel lives in canopy_pie.c rather than
// beside its scalar statement in garden.c so that it can be developed and run
// through the instruction model on its own: tools/pie/test_kernels.py
// (TestCanopyKernel.test_block) runs the assembly through piesim and compares all
// eight pixels of a block against the same arithmetic.
//
// The caller clips the ellipse and owns the head and tail. `n` counts blocks of
// eight, all of them inside |x - cx| <= rx, the pointer is the first pixel of the
// block, and x0 is that pixel's screen x.
void canopy_pie(uint16_t *row,int n,int cx,int mrr,int qy,uint16_t leafy,int x0);
