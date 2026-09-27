#ifndef POCKET_APP_MUSIC_VIEW_H
#define POCKET_APP_MUSIC_VIEW_H
#include "app_view_provider.h"

extern const pocket_app_view_provider pocket_app_music_view;
#ifdef KASANE_P0_PROBE
void pocket_app_music_view_probe_reset_counters(void);
#endif
#endif
