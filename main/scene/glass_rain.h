#pragma once
#include <stdint.h>

#define GLASS_RAIN_MAX_DROPS 6

/* One immutable rendering snapshot. Simulation may advance after capture;
 * drawing this value again must still produce the same pixels over the same
 * backdrop. The caller owns the copy until its display/repair work finishes. */
typedef struct {
    float x,y,start,speed,age,life,phase;
    int radius;
} glass_rain_drop;
typedef struct {
    glass_rain_drop drops[GLASS_RAIN_MAX_DROPS];
} glass_rain_frame;

// Screen-space background pass, called before the existing XMB overlay.
void glass_rain_prepare(float dt, uint32_t seed);
void glass_rain_capture(glass_rain_frame *out);
void glass_rain_draw_frame(const glass_rain_frame *frame,uint16_t *pixels,int y,int height);
// Compatibility entry for callers that render immediately after prepare.
void glass_rain_draw(uint16_t *pixels, int y, int height);
