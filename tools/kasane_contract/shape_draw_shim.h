#ifndef SHAPE_DRAW_SHIM_H
#define SHAPE_DRAW_SHIM_H
/* The script side of shape_draw_shim.c for MEGADEMO's harnesses. */
#define SHAPE_DRAW_JS "globalThis.__shapeDraw=(h,a)=>{if(WEMIT)pocket.kasane.procedural.draw(h,a);" \
    "else WD.push([h-1,a])}"
#endif
