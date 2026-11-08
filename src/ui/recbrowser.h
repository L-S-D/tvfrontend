/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Recordings browser (like recordings in the HTML5 app): top level grouped by title
 * (series as folders), plus the folder "Scheduled" with the timers;
 * on the right the details of the selected row. OK plays (with a saved position
 * asking resume/beginning) or opens a folder, Back goes up one
 * level or closes. Main thread only.
 */
#pragma once

#include "cJSON.h"

typedef struct {
    void (*request)(void);                         /* request recordings and timers */
    void (*play)(const char *id, int from_start); /* play recording (uuid) */
    /* timer action for a row from "Scheduled" (cancel, cancelSeries, recordSeries) */
    void (*timer_action)(const char *action, const cJSON *timer, int channel);
    void (*closed)(void); /* closed with Back on the top level */
} recbrowser_actions;

void recbrowser_init(const recbrowser_actions *a);

/* focus_id: select this recording (return from playback), NULL = keep selection */
void recbrowser_open(const char *focus_id);
void recbrowser_close(void);
int recbrowser_visible(void);

/* lists from tvrec (recordings / timers); takes ownership of the array */
void recbrowser_on_recordings(cJSON *list);
void recbrowser_on_timers(cJSON *list);

/* app_key; 1 = consumed (open: all keys) */
int recbrowser_key(int key);

/* pointer (ptr_event); open: consumes everything */
int recbrowser_pointer(int ev, int x, int y);

/* helpers for display elsewhere */
void rec_fmt_date(long t, char *out, size_t n);       /* "Fr 31.10. 14:25" */
void rec_fmt_duration(double s, char *out, size_t n); /* "1:02:03" / "12:34" */
