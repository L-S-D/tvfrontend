/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Channel list/EPG (full screen) as in the HTML5 app: all channels by group
 * (separator line with group name), Now/Next view or time grid (3 h),
 * group selection with Blue. The data comes from tvsrc (tvsrc_grid ->
 * grid with groupid "groups"), switching goes through tvsrc (tune).
 * Main thread only.
 */
#pragma once

#include "cJSON.h"

typedef struct {
    void (*request)(long start, long duration); /* request the grid for this time window */
    void (*tune)(int channel);
    void (*record)(int channel, double event_id); /* info/recording on a row (0 = no programme) */
} epg_actions;

void epg_init(const epg_actions *a);

/* Now/Next from now, the current channel selected */
void epg_open(int current_channel);
void epg_close(void);
int epg_visible(void);

/* epgGrid from tvsrc; takes ownership of msg (cJSON_Delete here) */
void epg_on_grid(cJSON *msg);

/* timers or groups changed: if open, reload, keep the selection */
void epg_refresh(void);

/* app_key (keys.h); 1 = consumed (open: all keys) */
int epg_key(int key);

/* pointer (ptr_event): pointing selects the row, a click on the channel column
 * switches, on a programme opens the recording dialog (like the
 * HTML5 app); open: consumes everything */
int epg_pointer(int ev, int x, int y);

/* Magic Remote wheel: dir > 0 = away from the user (upwards) */
void epg_wheel(int dir);

/* periodically: now line, progress */
void epg_tick(void);
