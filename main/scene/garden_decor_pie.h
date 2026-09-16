// The decor mix kernel's entry point. The kernel lives in
// scene/garden_decor_pie.c rather than beside its scalar statement in garden.c
// so that it can be developed and run through the instruction model on its own:
// tools/pie/test_kernels.py (GardenDecorMix.test_block) runs the assembly
// through piesim and compares all eight pixels of a block against the folded
// statement next to the kernel in that file.
//
// One call is eight pixels, from a 16-byte-aligned pointer (EE.VLD.128 forces
// the low four address bits to zero: a group that does not start on an
// eight-pixel boundary reads its neighbour), and light, shadow and d are the
// group's constants -- exactly the values garden_decor_row already evaluates
// once per group. The caller owns the clipping and the tails, and the group
// width has to be g_garden_decor_group = 8 for this to be the loop's arithmetic:
// at four columns the widest group is four pixels.
void garden_decor_mix8(uint16_t *row,int light,int shadow,int d);
