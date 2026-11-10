/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Programme source: channels, groups, events and recording entries directly
 * via HTSP from tvh (htsp.c), the channel state (current, previous) in the app.
 * Answers the UI's live/EPG requests (tvsrc_tune, _step,
 * _grid, _schedule ...) via tvsrc_actions. Recordings, timers and
 * resume positions are handled by tvrec.c (also HTSP). Live is always an
 * HTSP subscription (htsp://), also for IPTV channels; tvsrc builds the logos itself.
 * Main thread only.
 */
#pragma once

#include <stdint.h>

#include "cJSON.h"
#include "osd.h"
#include "tvrec.h"

typedef struct {
    char tvh_host[64];
    int htsp_port;           /* 9982 */
    int http_port;           /* 9981 (live stream) */
    char user[64], pass[64]; /* tvh user (HTSP, HTTP) */
    int dvr_read_only;       /* test: do not write timers/positions to tvh */
} tvsrc_cfg;

/* channel for the app: only valid during the call of act.play */
typedef struct {
    int channel;
    const char *name, *url, *icon; /* url: htsp://host:port/<channel> or IPTV directly */
    int initial;                   /* start or tvh reconnected */
    epg_entry now, next;
} tvsrc_play_info;

/* to the app (main thread); ownership of cJSON values passes to the receiver */
typedef struct {
    void (*play)(const tvsrc_play_info *p);
    void (*now_next)(const epg_entry *now, const epg_entry *next); /* current channel */
    void (*groups_changed)(void); /* groups or recording markers: reload the open EPG */
    /* EPG grid (reply to tvsrc_grid): groupid "groups", groups [{id,
     * label, start}], start, duration, currentChannel, channels [{id, name,
     * icon, broadcasts}] */
    void (*grid)(cJSON *grid);
    /* a channel's schedule from now (tvsrc_schedule, also after timer
     * changes): channel, name, icon, broadcasts (up to 12, 24 h) */
    void (*schedule)(cJSON *schedule);
    void (*connected)(int up); /* connection to tvh (ready or gone) */
    void (*noaccess)(void);    /* tvh refuses the user (login wrong or missing) */
} tvsrc_actions;

/* channel/last: remembered channel state (0 = none); ra: for tvrec */
void tvsrc_start(const tvsrc_cfg *c, const tvsrc_actions *a, const tvrec_actions *ra, int channel, int last);

/* play channel num, channel +1/-1 (dir), the previous one, the current one again */
void tvsrc_tune(int num);
void tvsrc_step(int dir);
void tvsrc_tune_last(void);
void tvsrc_tune_current(void);
/* refresh now/next of the current channel (act.now_next) */
void tvsrc_refresh_now_next(void);
/* EPG grid from start (0 = now) for duration s (0 = 3 h) -> act.grid */
void tvsrc_grid(int64_t start, int64_t duration);
/* schedule of channel num -> act.schedule */
void tvsrc_schedule(int num);

/* process messages from tvh, clean up (main loop) */
void tvsrc_poll(void);

/* channel state (for remembering) */
int tvsrc_channel(void);
/* HTSP channel ID for the reception info (tvhstat) of channel num: only
 * DVB channels (not IPTV, which may bypass tvh), otherwise 0 */
uint32_t tvsrc_status_channel(int num);

/* Event for the info window (current or next one of channel num). The
 * texts belong to tvsrc and are valid until the next tvsrc_poll. 1 = found */
typedef struct {
    uint32_t id;
    int64_t start, stop;
    const char *title, *subtitle, *summary, *text; /* text = long description */
    const char *onscreen;  /* episode according to the provider ("S02E05"), "" = none */
    char image[512];       /* ready-made URL, "" = no image */
    unsigned genre;        /* DVB contentType (0x10 movie ...), 0 = none */
    unsigned year, age, season, episode;
} tvsrc_event;
int tvsrc_event_info(int num, int next, tvsrc_event *out);
int tvsrc_last(void);

/* For Kodi/Kore (kodi.c), main thread. Groups (tvh tags) in tvh order:
 * ID and name (owned by tvsrc, valid until the next tvsrc_poll) */
int tvsrc_groups(uint32_t *ids, const char **names, int max);
int tvsrc_group_name(uint32_t tag, char *out, size_t n);
/* channel numbers of the group (tag 0 = all), by number; returns the count
 * (also beyond max) */
int tvsrc_channels(uint32_t tag, int *nums, int max);
int tvsrc_channel_meta(int num, char *name, size_t nn, char *icon, size_t ni);
/* known events of the channel, by start time */
int tvsrc_events(int num, tvsrc_event *out, int max);
/* event by HTSP ID, *num = channel */
int tvsrc_event_by_id(uint32_t id, tvsrc_event *out, int *num);
