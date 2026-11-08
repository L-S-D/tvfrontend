/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Recording dialog (like recDialog in the HTML5 app): schedule of a channel from
 * now (up to 12 programmes, from tvsrc_schedule), OK offers the timer actions (record/cancel,
 * series). Reachable with REC in live TV and Info/REC in the EPG.
 * Main thread only.
 */
#pragma once

#include "cJSON.h"

typedef struct {
    void (*request)(int channel);
    /* action: record | cancel | recordSeries | cancelSeries; b = programme
     * from channelSchedule (id, timerId, autorecId) */
    void (*action)(const char *action, const cJSON *b, int channel);
} recdlg_actions;

void recdlg_init(const recdlg_actions *a);
void recdlg_open(int channel, double focus_event_id); /* focus 0 = current programme */
void recdlg_close(void);
int recdlg_visible(void);

/* schedule of the channel (tvsrc_actions.schedule); takes ownership of msg */
void recdlg_on_schedule(cJSON *msg);

/* result of a timer action -> short message (text in out); returns 1 on error */
int recdlg_result_text(const char *action, int ok, const char *err, char *out, size_t n);

int recdlg_key(int key);

/* pointer (ptr_event); open: consumes everything */
int recdlg_pointer(int ev, int x, int y);
