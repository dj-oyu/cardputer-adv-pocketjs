#ifndef PROC_MEGADEMO_H
#define PROC_MEGADEMO_H

#include "ksn_procedural.h"
#include <math.h>
#include <string.h>

/* Five independent, bounded IR passes make one 240x135 RGB565 frame. The C
 * side selects scene parameters; all repeated geometry is drawn by the VM. */
#define PROC_MEGA_FRAMES 48u
#define PROC_MEGA_LAYERS 5u

static inline unsigned proc_mega_phase(unsigned frame) { return (frame % PROC_MEGA_FRAMES) / 16u; }
static inline uint16_t proc_mega_backdrop(unsigned frame) {
    static const uint16_t ink[3] = {0x080c, 0x000d, 0x100b};
    return ink[proc_mega_phase(frame)];
}

/* Playback changes only these bounded inputs; no IR construction or plan
 * preparation is needed after registering the five passes for each scene. */
static inline bool proc_mega_inputs(unsigned frame, unsigned layer,
                                    float input[KSN_PROC_INPUTS]) {
    if (!input || layer >= PROC_MEGA_LAYERS) return false;
    const unsigned phase = proc_mega_phase(frame);
    const float beat = (float)(frame % 16u);
    const float pulse = sinf(beat * 0.3926990817f);
    memset(input,0,KSN_PROC_INPUTS * sizeof input[0]);
    if (layer == 0) {
        const float inset = phase == 1 ? 19.0f : (phase == 2 ? 3.0f : 8.0f);
        const float sway = pulse * (phase == 2 ? 9.0f : 3.0f);
        input[0] = inset + sway;
        input[1] = 239.0f - inset + sway;
    } else if (layer == 1) {
        input[0] = -18.0f + pulse * 4.0f;
        input[1] = 91.0f + pulse * 3.0f;
    } else if (layer == 2) {
        input[0] = (phase == 1 ? 70.0f + pulse : 65.0f + pulse * 2.0f);
    } else if (layer == 3) {
        input[0] = beat * 0.31f;
    } else {
        input[0] = (phase == 2 ? 9.0f : 17.0f) + (float)((frame * 7u) % 19u);
        input[1] = (float)((int)(frame % 5u) - 2) * (phase == 2 ? 2.0f : 1.0f);
    }
    return true;
}

static inline bool proc_mega_build(unsigned frame, unsigned layer,
                                    ksn_proc_inst code[KSN_PROC_CODE],
                                    ksn_proc_program *program,
                                    float input[KSN_PROC_INPUTS]) {
    if (!code || !program || !input || layer >= PROC_MEGA_LAYERS) return false;
    const unsigned phase = proc_mega_phase(frame);
    const uint16_t neon[3][4] = {
        {0x07ff, 0x3dff, 0xb81f, 0xffff}, /* cyan tunnel */
        {0xfde0, 0xfb00, 0x07ff, 0xffff}, /* amber highway */
        {0xf81f, 0x781f, 0x07ff, 0xffff}  /* magenta gate */
    };
    unsigned n = 0;
#define EMIT(OP,D,A,B,V,C) do { \
    if (n == KSN_PROC_CODE) return false; \
    memset(&code[n],0,sizeof code[n]); \
    code[n].op=(OP); code[n].dst=(D); code[n].a=(A); code[n].b=(B); \
    code[n].value=(V); code[n].color=(C); n++; \
} while (0)
#define SET(R,V) EMIT(KSN_PROC_SET,(R),0,0,(V),0)
#define INPUT(R,I) EMIT(KSN_PROC_INPUT,(R),(I),0,0,0)
#define MOVE(X,Y) EMIT(KSN_PROC_MOVE,0,(X),(Y),0,0)
#define LINE(X,Y,C) EMIT(KSN_PROC_LINE,0,(X),(Y),0,(C))
#define ADD(D,A,B) EMIT(KSN_PROC_ADD,(D),(A),(B),0,0)
#define MUL(D,A,B) EMIT(KSN_PROC_MUL,(D),(A),(B),0,0)
    if (layer == 0) {
        /* Animated receding rectangular portals. Their repeated outline is
         * generated entirely by the loop; all eight registers are occupied. */
        INPUT(0,0); INPUT(1,1);
        SET(2, phase == 1 ? 42.0f : 5.0f);
        SET(3, phase == 1 ? 125.0f : 129.0f);
        SET(4, phase == 1 ? 13.0f : 12.0f); SET(5, phase == 1 ? -13.0f : -12.0f);
        SET(6, phase == 1 ? 3.0f : 7.0f); SET(7, -7.0f);
        EMIT(KSN_PROC_REPEAT,0,phase == 1 ? 6 : 8,0,0,0);
        MOVE(0,2); LINE(1,2,neon[phase][0]); LINE(1,3,neon[phase][0]);
        LINE(0,3,neon[phase][0]); LINE(0,2,neon[phase][0]);
        ADD(0,0,4); ADD(1,1,5); ADD(2,2,6); ADD(3,3,7);
        EMIT(KSN_PROC_END,0,0,0,0,0);
    } else if (layer == 1) {
        /* Vanishing-point rays. Each iteration moves both the near and far
         * endpoint, making the road or warp tunnel read as 3D. */
        INPUT(0,0); INPUT(1,1);
        SET(2, phase == 1 ? 64.0f : 68.0f); SET(3, 134.0f);
        SET(4, 23.0f); SET(5, 4.8f);
        EMIT(KSN_PROC_REPEAT,0,12,0,0,0);
        MOVE(1,2); LINE(0,3,neon[phase][1]);
        ADD(0,0,4); ADD(1,1,5);
        EMIT(KSN_PROC_END,0,0,0,0,0);
    } else if (layer == 2) {
        /* Depth grid: exponentially growing gaps give the horizon motion. */
        SET(0,0.0f); SET(1,239.0f);
        INPUT(2,0);
        SET(3,1.5f); SET(4,1.47f);
        EMIT(KSN_PROC_REPEAT,0,10,0,0,0);
        MOVE(0,2); LINE(1,2,neon[phase][2]);
        ADD(2,2,3); MUL(3,3,4);
        EMIT(KSN_PROC_END,0,0,0,0,0);
    } else if (layer == 3) {
        /* A sinuous oscilloscope trace becomes the glitch signal. Adjacent
         * MUL/ADD instructions deliberately exercise a compiled def-use span. */
        SET(0,0.0f); SET(1,4.0f); SET(2,0.073f + (float)phase * 0.015f);
        INPUT(3,0); SET(4,phase == 1 ? 7.0f : 13.0f);
        SET(5,phase == 2 ? 42.0f : 28.0f);
        EMIT(KSN_PROC_REPEAT,0,60,0,0,0);
        MUL(6,0,2); ADD(6,6,3);
        EMIT(KSN_PROC_SIN,6,6,0,0,0);
        MUL(7,6,4); ADD(7,7,5);
        LINE(0,7,neon[phase][3]);
        ADD(0,0,1);
        EMIT(KSN_PROC_END,0,0,0,0,0);
    } else {
        /* The same registered program is reused for a whole scene. INPUTs
         * slide paired, differently colored scanlines through the image. */
        INPUT(0,0); SET(1,29.0f);
        SET(2,0.0f); SET(3,239.0f);
        INPUT(4,1);
        SET(6,2.0f);
        EMIT(KSN_PROC_REPEAT,0,4,0,0,0);
        MOVE(2,0); LINE(3,0,neon[phase][2]);
        ADD(5,0,6); MOVE(2,5); LINE(3,5,neon[phase][0]);
        ADD(0,0,1); ADD(2,2,4); ADD(3,3,4);
        EMIT(KSN_PROC_END,0,0,0,0,0);
    }
    if (!proc_mega_inputs(frame,layer,input)) return false;
    program->code = code;
    program->count = (uint8_t)n;
#undef EMIT
#undef SET
#undef INPUT
#undef MOVE
#undef LINE
#undef ADD
#undef MUL
    return true;
}

#endif
