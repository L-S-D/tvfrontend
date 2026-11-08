/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Kodi JSON-RPC in TVF (for Kore and Home Assistant): HTTP (POST/GET
 * /jsonrpc, GET /image/... = logos and event images from the tvh image cache)
 * and TCP (JSON-RPC and the notifications Player.OnPlay/OnStop/OnPause/
 * OnResume). Kore sees TVF as a Kodi with channel list, EPG, recordings and
 * timers (from tvsrc/tvrec) and controls zapping, recordings, pause/seeking
 * and the keys; Home Assistant switches channels e.g. with Player.Open
 * {"item":{"channelid":<channel number>}}.
 *
 * A network thread accepts requests and writes replies; evaluation happens
 * in the main thread (kodi_poll).
 */
#pragma once

typedef struct {
    int (*channel)(void);     /* current channel, 0 = none */
    int (*live_paused)(void); /* live paused (timeshift) */
    void (*tune)(int num);
    void (*step)(int dir); /* channel +1 / -1 */
    void (*stop)(void);    /* end playback (recording: back to live) */
    void (*key)(int app_key); /* key as if from the remote control */
    void (*request)(void);    /* a request from Kore arrived (including the polling every few s) */
    void (*front)(void);      /* zapping/recording from Kore: bring TVF to the foreground */
} kodi_actions;

typedef struct {
    char name[64];          /* name in Kore */
    int http_port, tcp_port; /* 0 = off */
    /* login like Kodi (HTTP Basic), "" = none. TCP (no login with Kodi)
     * only from addresses that have logged in via HTTP (remembered in prefs
     * "kore.trusted", expires with new credentials) */
    char kore_user[64], kore_pass[64];
    /* image proxy: only this tvh's image cache, with its credentials */
    char tvh_host[64];
    int tvh_http_port;
    char user[64], pass[64];
} kodi_cfg;

/* wake: wakes the main thread (then kodi_poll). 0 = running */
int kodi_start(const kodi_cfg *c, const kodi_actions *a, void (*wake)(void));

/* answer requests, report playback changes (main loop) */
void kodi_poll(void);
