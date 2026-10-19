#include "derby_telemetry.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static char lines[310][768];
static unsigned count;
static bool refuse;
bool derby_telemetry_write(const char *line, size_t length) {
    assert(length < 768 && length && line[length-1]=='\n');
    assert(count < 310);
    memcpy(lines[count],line,length); lines[count++][length]=0;
    return !refuse;
}
static derby_telemetry_context c={.seed=2,.race=1,.phase=2,.camera=8,.tier=1,.step=391,.u_ppm=500000};
static void frame(int64_t now, size_t used) {
    derby_telemetry_begin_frame(); derby_telemetry_set(c); derby_telemetry_present(now,used,163840);
}
int main(void) {
    derby_telemetry_reset(); derby_telemetry_eval(120000,163840);
    frame(1000000,125000);
    derby_telemetry_present(1100000,999999,163840); /* repeated transfer: no sample */
    frame(1500000,126000); assert(!count);
    frame(2000000,127000); assert(count==1);
    assert(strstr(lines[0],"frame=3 ctx=3") && strstr(lines[0],"gu_sample_max=127000"));
    assert(strstr(lines[0],"eval_used=120000 eval_cap=163840"));
    assert(strstr(lines[0],"2:1000000")); /* two same-camera intervals */
    /* Missing render contexts and camera changes cannot inflate valid fps. */
    derby_telemetry_begin_frame();
    /* B parked/failed before completion. A's redraw must not acquire B. */
    derby_telemetry_present(2200000,999999,163840);
    c.camera=9; frame(2500000,126000);
    c.camera=8; refuse=true; frame(3000000,126000); assert(count==2);
    assert(strstr(lines[1],"2:1000000"));
    refuse=false; frame(4000000,128000); assert(count==3);
    assert(strstr(lines[2],"refused=1 seq=3"));
    /* Across runs the global rate ceiling remains one output/second. */
    derby_telemetry_reset(); frame(4100000,120000); frame(4500000,120000);
    assert(count==3); frame(5100000,120000); assert(count==4);
    assert(strstr(lines[3],"run=2") && strstr(lines[3],"eval_cap=0"));
    for(unsigned i=1;i<305;++i) frame(5100000+(int64_t)i*1000000,120000);
    assert(count==303); assert(strstr(lines[count-1],"seq=300 limit=300"));
    puts("PASS bounded sampled telemetry: rate, context gaps, cameras, refusal, reset, limit");
}
