/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Media server for WebKit's own playback (tvf-web with --media, bundle with
 * "media" in features): the TVF sinks in WebKit's GStreamer pipeline send
 * the still encoded streams here (protocol: wpe-webos PROTOCOL-MEDIA.md),
 * TVF feeds them into the TV's decoder (StarfishMediaAPIs, BUFFERSTREAM like
 * the live player) in TVF's video window and reports position and end back.
 * WebKit does everything else itself: MSE, buffering, seeking, tracks.
 *
 * Threads: accept, one reader per connection, one feeder per session (load,
 * feed, play/pause). Creating, unloading and destroying the pipeline as well as
 * live picture and video window in the main thread (webmedia_poll), as with the live player.
 */
#pragma once

typedef struct {
    void (*live)(int on);                      /* live picture off (0, releases the decoder) or back on (1) */
    void (*video)(int x, int y, int w, int h); /* video window 1920x1080, w = 0: full screen */
    const char *(*window_id)(void);            /* TVF's exported video window */
    void (*source)(int w, int h);              /* decoder picture size (VIDEO_INFO), 0 = live picture */
} webmedia_actions;

/* Open the socket; view_w = width of tvf-web's view (page pixels ->
 * 1920). 0 = running */
int webmedia_init(const webmedia_actions *a, void (*wake)(void), int view_w);

/* Name for TVF_MEDIA_SOCK ("@tvf-media-<pid>"), "" = not running */
const char *webmedia_socket(void);

/* transmission.contentsType of the load ("LIVE" like the live player, "none" = omit) */
void webmedia_set_ctype(const char *ctype);

/* a playback is running (or is being torn down) */
int webmedia_active(void);

/* Main loop: create/tear down pipelines, live picture, video window */
void webmedia_poll(void);
