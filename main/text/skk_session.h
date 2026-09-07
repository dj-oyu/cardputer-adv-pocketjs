#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ime_core.h"

// The device's one IME session: the dictionary mapped out of the skk_dict
// partition, plus the ime_t that owns it. A skk_t is single-task by contract,
// so only the UI task may touch this.
bool skk_session_init(void);
bool skk_session_ready(void);
ime_t *skk_session(void);
const char *skk_session_status(void);
void skk_session_timing(int64_t *open_us, int64_t *probe_us);
