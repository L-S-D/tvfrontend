/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Timeshift in live TV: tvh's timeshift in the running HTSP subscription -
 * pause/resume (subscriptionSpeed), jumps (subscriptionSkip) and back
 * to live (subscriptionLive); the distance to live comes from tvh
 * (timeshiftStatus). Main thread only.
 */
#pragma once

#include "player.h"

void tshift_init(player *pl);

/* channel change (play message): live_url (htsp:// = timeshift from tvh, also
 * for returning after an abort); enabled 0 = no timeshift */
void tshift_on_play(const char *live_url, int enabled);

/* recording/background: forget pause and jump */
void tshift_stop(void);

/* paused or clearly behind live: OK/Left/Right/Back belong to the timeshift */
int tshift_active(void);

/* channel (presumably) has timeshift */
int tshift_available(void);

void tshift_toggle_pause(void);
void tshift_seek(int delta_s);
void tshift_live(void);

void tshift_tick(void);
