#pragma once
#include <stdint.h>
// The host supplies the clock. A test that has to prove a timeout fires cannot
// wait for a real one, so the definition lives in the test and moves when the
// test says it does.
int64_t esp_timer_get_time(void);
