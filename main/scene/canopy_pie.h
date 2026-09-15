// The canopy kernel's entry point. The kernel lives in canopy_pie.c rather than
// beside its scalar statement in garden.c so that it can be developed and run
// through the instruction model on its own; the arithmetic it rests on is proven
// in tools/pie/models/canopy_model.c and the assembly is checked against the
// same model in tools/pie/test_kernels.py.
//
// The caller clips the ellipse and owns the head and tail. `n` counts blocks of
// eight, all of them inside |x - cx| <= rx, and x0 is the screen x of row[0].
void canopy_pie(uint16_t *row,int n,int cx,int mrr,int qy,uint16_t leafy,int x0);
