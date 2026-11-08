/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Recordings, timers and resume positions directly via HTSP.
 * Keeps tvh's dvrEntry and autorecEntry messages (via tvsrc) and
 * answers the app's requests (results via tvrec_actions):
 *   tvrec_request_list -> recordings or timers (on changes pushed again,
 *     batched, once the app has asked)
 *   tvrec_action -> result: record (addDvrEntry), cancel (cancel- or
 *     stopDvrEntry), recordSeries (addAutorecEntry: same title, same
 *     channel), cancelSeries (deleteAutorecEntry)
 *   tvrec_play_uuid / tvrec_play -> play with fileUrl (/dvrfile, tvh credentials)
 *   tvrec_position -> updateDvrEntry playposition (end or < 2 min left:
 *     watched = playcount 1, position 0);
 *     if tvh rejects this (user without write access to other users' recordings),
 *     the position stays in the TV's settings ("rec.local")
 * Main thread only.
 */
#pragma once

#include "cJSON.h"

#include <stddef.h>
#include <stdint.h>

/* results to the app (main thread); ownership of the cJSON values passes to the receiver */
typedef struct {
    void (*recordings)(cJSON *list); /* recordings, newest first */
    void (*timers)(cJSON *list);     /* timers, earliest first */
    void (*play)(cJSON *item);       /* play a recording: entry with fileUrl and position */
    /* result of a timer action without its own done (err "" = none) */
    void (*timer_result)(const char *action, int ok, const char *err);
} tvrec_actions;

typedef struct {
    tvrec_actions act;
    /* HTSP channel -> number, name, logo URL; 0 = unknown */
    int (*channel)(uint32_t chid, int *num, char *name, size_t nn, char *icon, size_t ni);
    /* event -> title and HTSP channel; 0 = unknown */
    int (*event)(uint32_t id, char *title, size_t n, uint32_t *chid);
    void (*schedule)(int num); /* resend the channel's schedule (timer markers) */
    char host[64];
    int http_port;
    char user[64], pass[64];
    int read_only; /* test: write nothing to tvh, only log */
} tvrec_cfg;

void tvrec_init(const tvrec_cfg *c);

/* before every (re)sync */
void tvrec_clear(void);

/* asynchronous HTSP message (dvrEntry*, autorecEntry*; others are ignored) */
void tvrec_htsp(const cJSON *m);

/* reply to one of our own requests: 1 = handled */
int tvrec_reply(int seq, const cJSON *m);

/* request a list (timers: 0 recordings, 1 timers); arrives via act.recordings
 * or act.timers, afterwards automatically on changes */
void tvrec_request_list(int timers);

/* report the playback position of a recording (uuid): written rarely,
 * immediately on pause, stop and end */
void tvrec_position(const char *uuid, double pos, double dur, int paused, int stopped, int ended);

/* play a recording (act.play): 0 = unknown */
int tvrec_play_uuid(const char *uuid, int from_start);

/* timer action (record, cancel, recordSeries, cancelSeries) for event event,
 * timer timer or series autorec; the result arrives via done (possibly immediately),
 * without done via act.timer_result. channel > 0: resend its schedule */
void tvrec_action(const char *action, uint32_t event, uint32_t timer, const char *autorec, int channel,
                  void (*done)(void *user, int ok, const char *err), void *user);
/* play a recording by HTSP ID (act.play): 0 = unknown */
int tvrec_play(uint32_t num, int from_start);

/* push changed lists (main loop) */
void tvrec_poll(int synced);

/* ---- for Kodi/Kore (kodi.c) ---- */

/* recordings (newest first) or timers (earliest first) as for
 * act.recordings/act.timers; owned by the caller */
cJSON *tvrec_list(int timers);
/* recording by HTSP ID (as in the list) or NULL; owned by the caller */
cJSON *tvrec_recording(uint32_t num);
/* scheduled/running timer of the event: 1 = present */
int tvrec_timer_of_event(uint32_t event, uint32_t *timer, char *autorec, size_t n, int *recording);
