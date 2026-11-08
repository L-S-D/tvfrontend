/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Subtitles (teletext and DVB) like subs in the HTML5 app: the events come
 * from the HTSP subscription's own store (sidedata.c, "local:ttx/subtitles"
 * or "local:ttx/dvbsub"), each with the stream PTS from
 * which it applies. Natively we know the PTS of the displayed picture
 * directly (player_stream_pts) - no detour via a time base.
 * Selection id: teletext "777", DVB "dvb:<pid>:<page>". Main thread only.
 */
#pragma once

#include <stddef.h>

typedef struct {
    long long (*now_pts)(void); /* displayed stream PTS (90 kHz) or -1 */
} subs_actions;

#define SUBS_MAX 12

typedef struct {
    char id[32];
    char label[96];
} subs_option;

void subs_init(const subs_actions *a);

/* channel change: local 1 = store of the HTSP subscription (sidedata.c), 0 = no
 * subtitles (IPTV); channel for the remembered choice. Fetches the list, starts
 * the remembered choice. */
void subs_set_source(int local, int channel);

/* available subtitles of the channel; *current = index of the chosen one (-1 = off) */
int subs_options(subs_option *out, int max, int *current);

/* NULL = off; remembered per channel */
void subs_choose(const char *id);

/* on (id) or off (NULL) only for this session, without the remembered choice
 * (HbbTV selectComponent) */
void subs_select_session(const char *id);

/* subtitles on? */
int subs_enabled(void);

/* offset to the picture (like the HTML5 app): + = subtitles later, remembered
 * per channel, without its own value the last one set applies. Adjust with
 * Left/Right (25 ms), OK/Back done. */
void subs_offset_label(char *out, size_t n);
void subs_adjust_start(void);
int subs_adjusting(void);
int subs_adjust_key(int key); /* app_key; 1 = consumed */

void subs_tick(void);
