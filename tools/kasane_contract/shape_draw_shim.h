#ifndef SHAPE_DRAW_SHIM_H
#define SHAPE_DRAW_SHIM_H
/* The script side of shape_draw_shim.c for MEGADEMO's harnesses: a segment
 * goes to the procedural mock's frame when one is being drawn (the frame
 * loop's beginFrame), else into WAKE's list as plan -1, a raw segment
 * [x0, y0, x1, y1, color]. */
#define SHAPE_DRAW_JS "globalThis.__shapeSeg=(x0,y0,x1,y1,c)=>{" \
    "const s=[x0,y0,x1,y1,c];if(WEMIT)pocket.kasane.procedural.__seg(s);else WD.push([-1,s])}"
#endif
