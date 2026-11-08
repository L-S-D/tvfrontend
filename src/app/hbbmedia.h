/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Catch-up ("Mediathek") playback for HbbTV pages in the renderer of their
 * own: WebKit plays nothing itself, the page hands <video>/<audio> over to TVF
 * (res/hbbtv/media.js). Here the TV's media server (uMediaServer,
 * luna://com.webos.media) loads the address itself - MP4, HLS, DASH - and shows
 * it in TVF's video window; the live picture is paused meanwhile.
 *
 * State goes back to the page via act.update. Main thread only
 * (the Luna replies arrive on a foreign thread and are queued
 * here).
 */
#pragma once

typedef struct {
    /* live picture off (0, releases the decoder) or back on (1) */
    void (*live)(int on);
    /* State for element id: state "playing", "paused", "buffering",
     * "ended", "error" or NULL (time/duration only); time/dur in s, < 0 =
     * unchanged; code = error code */
    void (*update)(int id, const char *state, double time, double dur, int code);
    /* TVF's exported video window */
    const char *(*window_id)(void);
} hbbmedia_actions;

void hbbmedia_init(const hbbmedia_actions *a, void (*wake)(void));

/* HLS is played by TVF's own player (the TV's media server hangs on HLS) */
struct player;
void hbbmedia_set_player(struct player *p);

/* subsequent loads with hbbdrm's PlayReady client (option.drm) */
void hbbmedia_drm(int on);
/* Test aid: mediaTransportType back inside option instead of next to it */
void hbbmedia_transport_in_option(int on);

/* Element id plays url (mime may be empty), from start s */
void hbbmedia_play(int id, const char *url, const char *mime, double start);
void hbbmedia_resume(int id);
void hbbmedia_pause(int id);
void hbbmedia_seek(int id, double sec);
/* Stop playback of id; the live picture returns after a short quiet period
 * (ad -> programme item without a live interlude) */
void hbbmedia_stop(int id);
/* Stop everything, live picture immediately (HbbTV closed) */
void hbbmedia_stop_all(void);

int hbbmedia_active(void);

void hbbmedia_poll(void);
