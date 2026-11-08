/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * HbbTV in a renderer of its own (tvf-web, webr.c) instead of in the overlay
 * of the HTML5 app: Red fetches the channel's AIT from the HTSP subscription,
 * loads the autostart or present app with the gateway's OIPF replacement as
 * a user script and places the page's image between the live picture and the
 * UI. Keys go to the page according to the app's keyset, the rest to TVF;
 * the replacement's messages (keyset, video window, end, ...) arrive via
 * window.webkit.messageHandlers.tvf. The logic follows the overlay
 * (TvhWebOsClient js/app.js, hbbtv).
 *
 * Egress (geo/VPN) goes through an HTTP proxy for all of the page's requests,
 * no gateway session. Main thread only.
 */
#pragma once

#include "cJSON.h"

typedef struct {
    void (*video)(int x, int y, int w, int h); /* live picture into this window (1920x1080), w = 0: full screen */
    void (*closed)(void);                      /* HbbTV closed */
    int (*dvb_ids)(int *onid, int *tsid, int *sid); /* DVB IDs of the live stream, 1 = known */
    /* TV picture behind the app on/off: the page stops it (video/broadcast
     * stop(), setChannel(null), start of a broadcast-independent app via http)
     * or brings it back (bindToCurrentChannel(), setChannel(channel)); on
     * close it is on again (closed) */
    void (*broadcast)(int on);
    /* Components of the live stream for video/broadcast getComponents()
     * (OIPF AVComponent): JSON array (malloc) or NULL; fields kind (0 video,
     * 1 audio, 2 subtitles), index, tag (component_tag), enc, lang, ad, hi, ch,
     * sub (subtitle ID for component), active */
    char *(*components)(void);
    /* selectComponent/unselectComponent of the page: audio (index) or
     * subtitles (sub) on/off */
    void (*component)(int kind, int index, const char *sub, int on);
} hbbweb_actions;

typedef struct {
    char bundle[256];     /* tvf-web bundle (directory with run.sh) */
    char data_dir[256];   /* cookies, cache, font cache (persistent) */
    char proxy[160];      /* egress for all of the page's requests, "" = direct */
    char polyfill[600];   /* res/hbbtv/polyfill.js: OIPF replacement */
    char dash[256];       /* dash.js for A/V objects when the relay is not running */
    char user_agent[256]; /* with "HbbTV/1.x" (broadcasters' device detection) */
    char media_js[600];   /* res/hbbtv/media.js: the page's <video> -> TVF (hbbmedia.c) */
    char drm_js[600];     /* res/hbbtv/drm.js: OIPF DRM agent -> LG's PlayReady (hbbdrm.c) */
    char media_sock[64];  /* media server (webmedia.c): WebKit plays by itself if the bundle
                           * supports "media"; "" = media.js emulation */
} hbbweb_cfg;

void hbbweb_init(const hbbweb_cfg *cfg, const hbbweb_actions *a, void (*wake)(void));

/* Start the helper early (saves the start on the first Red) */
void hbbweb_prestart(void);

/* Red: load the channel's app (AIT from the HTSP subscription). rewrites = JSON array
 * [[regex, replacement], ...] (entry per app, hbbtv.c) or NULL.
 * 0 = no HbbTV path (no renderer) */
int hbbweb_open(int channel, const char *name, const char *rewrites);

/* after dwelling on a channel: preload its app invisibly (keys stay with
 * TVF, hbbweb_active() is 0); Red (hbbweb_open) then shows it immediately.
 * A channel change/hbbweb_close discards it */
void hbbweb_preload(int channel, const char *name, const char *rewrites);

/* after dwelling, with the key mapping "HbbTV first": start the channel's app
 * visibly like an HbbTV TV set - without the opening Red (a red-button app
 * shows its hint), without messages on errors and, until its keyset, only
 * with Red; the wheel keeps zapping as long as it does not want arrow keys.
 * If it takes a key, it counts as opened with Red */
void hbbweb_autostart(int channel, const char *name, const char *rewrites);

/* does the visible app take this key (app_key, according to the keyset)? */
int hbbweb_takes(int key);

/* opened (also while the app is still loading) */
int hbbweb_active(void);

/* Key (app_key from keys.h): 1 = went to the app or closed HbbTV,
 * 0 = belongs to TVF (CH+/- close HbbTV and still belong to TVF) */
int hbbweb_key(int key);

/* Pointer (ptr_event from keys.h, window coordinates): 1 = went to the page as
 * a mouse (only with keyset.supportsPointer), 0 = TVF decides (click = OK) */
int hbbweb_pointer(int ev, int x, int y);
void hbbweb_pointer_gone(void); /* pointer hidden */

/* Wheel: 1 = HbbTV has it (open: never a channel change), 0 = belongs to TVF */
int hbbweb_wheel(int dir);

void hbbweb_close(const char *why);

/* Media in WebKit on (sock = media server) or off (NULL): applies from the
 * next start of the renderer (if it is running without an open page, it is stopped) */
void hbbweb_set_media(const char *sock);

/* State of media playback to the page (for hbbmedia_actions.update) */
void hbbweb_media_update(int id, const char *state, double time, double dur, int code);

/* Helper messages, delayed Red (main loop) */
void hbbweb_poll(void);
