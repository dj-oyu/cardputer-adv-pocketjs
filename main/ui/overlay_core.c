#include "overlay_core.h"

bool overlay_region_holds(const overlay_region_t *region,
                          int x, int y, int w, int h) {
    if(!region) return false;
    if(w<=0 || h<=0) return false;
    if(x<0 || y<0) return false;
    // Widened before adding, so a program that asks for INT_MAX gets a refusal
    // rather than a wrapped sum that happens to land inside the box.
    long long right=(long long)x+(long long)w;
    long long bottom=(long long)y+(long long)h;
    return right<=region->w && bottom<=region->h;
}

overlay_state_t overlay_boot_state(bool armed, bool starting) {
    if(!armed) return OVERLAY_OFF;
    // The order is the valve. A device that is armed AND still carries the
    // flag boots WITHOUT the overlay, whatever else is true, and stays that
    // way until somebody turns it off and on again by hand.
    return starting ? OVERLAY_BLOCKED : OVERLAY_STARTING;
}

bool overlay_room_left(uint32_t free_after, uint32_t floor) {
    // At the floor is still inside it. The floor is a documented requirement
    // of somebody else's -- the radio's -- and meeting it exactly is meeting
    // it; refusing there would be inventing a margin on top of a measurement.
    return free_after>=floor;
}

void overlay_budget_start(overlay_budget_t *b, uint64_t now_us) {
    b->over_run=0; b->started_us=now_us;
    b->last_us=0; b->worst_us=0; b->turns=0;
}

bool overlay_budget_turn(overlay_budget_t *b, uint32_t turn_us) {
    b->last_us=turn_us; b->turns++;
    if(turn_us>b->worst_us) b->worst_us=turn_us;
    if(turn_us<=b->budget_us) { b->over_run=0; return false; }
    if(b->over_run<0xffffu) b->over_run++;
    return b->over_limit && b->over_run>=b->over_limit;
}

bool overlay_budget_healthy(const overlay_budget_t *b, uint64_t now_us) {
    // Both halves: long enough, and not in the middle of a bad run. A start
    // that was over budget from its first turn is exactly what should not be
    // declared healthy at the five second mark.
    return now_us-b->started_us>=b->healthy_us && b->over_run==0;
}
