#ifndef SHAPE_DRAW_SHIM_H
#define SHAPE_DRAW_SHIM_H
/* The script side of shape_draw_shim.c for MEGADEMO's harnesses: a segment
 * [x0, y0, x1, y1, color] goes to the procedural mock's frame when the frame
 * loop draws (the app's E is its draw, which names its 8 inputs: length 9),
 * else into procMegademo.frame()'s list as plan -1 (E collects a rest). */
#define SHAPE_DRAW_JS "globalThis.__shapeSeg=(x0,y0,x1,y1,c)=>{" \
    "const P=pocket.kasane&&pocket.kasane.procedural;" \
    "if(P&&P.__seg&&E.length>1)P.__seg([x0,y0,x1,y1,c]);else E(-1,x0,y0,x1,y1,c)}"
#endif
