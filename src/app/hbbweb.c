/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hbbweb.h"
#include "i18n.h"

#include "egress.h"
#include "fetch.h"
#include "hbbdrm.h"
#include "hbbmedia.h"
#include "keys.h"
#include "log.h"
#include "osd.h"
#include "relay.h"
#include "sidedata.h"
#include "ui_sdl.h"
#include "webr.h"

#include <SDL.h>
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define PAGE_W 1280 /* HbbTV pages are built for 1280x720 */
#define PAGE_H 720
#define SCREEN_W 1920 /* TVF's window: pointer coordinates */
#define SCREEN_H 1080
#define RED_SETTLE_MS 1200 /* replay Red when the keyset has been quiet this long ... */
#define RED_MAX_MS 6000    /* ... at most this long after the first keyset with Red */
#define LAUNCH_EXIT_MS 5000
/* the app ends or hides right after the replayed Red: it was already showing
 * its interface (Anixe+: Red = destroyApplication) - start it again without */
#define RED_UNDO_MS 3000
#define NO_REPLAY_MAX 8

static hbbweb_cfg cfg;
static hbbweb_actions act;

static struct {
    int visible;  /* opened (app loading or running) */
    int waiting;  /* Red pressed, AIT/replacement/helper not there yet */
    int bg;       /* preloaded (dwelling on the channel): runs invisibly, keys with TVF, Red shows it */
    int gen;      /* counter for late replies */
    int channel;
    char name[96];
    char app_name[96];
    char url[1024];   /* loaded app */
    char *rewrites;   /* JSON array (hbbtv.c) or NULL */
    cJSON *ait;       /* apps of the AIT {org, app, control, url, name} */
    int ait_done, ait_err;
    int ait_tries;
    uint32_t ait_retry_at; /* AIT not yet in the subscription - ask again */
    int keyset, keyset_set, hidden;
    int pointer;      /* page is operated by pointer (keyset.supportsPointer) */
    double ptr_x, ptr_y; /* last pointer position in the page, < 0 unknown */
    uint32_t shown_at, launch_at;
    int pending_red;
    int autostart;    /* started by itself (key mapping "HbbTV first"): without Red, quiet */
    uint32_t red_since, red_at;
    uint32_t red_replayed_at; /* Red passed on to the starting app (undo window) */
    uint32_t back_times[3];
    int layer;        /* image is placed in the UI */
    int vrect[4];     /* last reported video window (skip repetitions) */
} S;

/* Move the live picture only on change (the replacement reports continuously) */
static void video(int x, int y, int w, int h) {
    if (S.vrect[0] == x && S.vrect[1] == y && S.vrect[2] == w && S.vrect[3] == h) return;
    S.vrect[0] = x;
    S.vrect[1] = y;
    S.vrect[2] = w;
    S.vrect[3] = h;
    mt_log("[hbbweb] video window %d,%d %dx%d", x, y, w, h);
    act.video(x, y, w, h);
}

static char *g_polyfill; /* res/hbbtv/polyfill.js: OIPF replacement */
static char *g_media_js; /* res/hbbtv/media.js */
static char *g_drm_js;   /* res/hbbtv/drm.js */
static int g_drm_used;   /* page has used the DRM agent or DRM data */
static int g_hole;       /* renderer punches the video hole itself (features: video-hole) */
static int g_can_media;  /* renderer can do media itself (features: media, WebKit patch 0009) */
static int g_eme;        /* EME via TVF's CDM host (features: eme, libocdm in the bundle) */
static int g_native;     /* ... and does it: tvf-web --media, data to webmedia.c, media.js only in selective mode */

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : NULL;
}

static double jnum(const cJSON *o, const char *k, double def) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : def;
}

/* TVF key -> DOM keyCode (HbbTV/OIPF), 0 = none */
static int dom_code(int key) {
    if (KEY_IS_DIGIT(key)) return 48 + (key - KEY_DIGIT0);
    switch (key) {
    case KEY_UP: return 38;
    case KEY_DOWN: return 40;
    case KEY_LEFT: return 37;
    case KEY_RIGHT: return 39;
    case KEY_OK: return 13;
    case KEY_BACK: return 461;
    case KEY_RED: return 403;
    case KEY_GREEN: return 404;
    case KEY_YELLOW: return 405;
    case KEY_BLUE: return 406;
    case KEY_CH_UP: return 427;
    case KEY_CH_DOWN: return 428;
    case KEY_INFO: return 457;
    case KEY_PLAY: return 415;
    case KEY_PAUSE: return 19;
    case KEY_PLAYPAUSE: return 463;
    case KEY_STOP: return 413;
    case KEY_REWIND: return 412;
    case KEY_FFWD: return 417;
    default: return 0;
    }
}

/* OIPF keyset: RED 0x1, GREEN 0x2, YELLOW 0x4, BLUE 0x8, NAVIGATION 0x10,
 * VCR 0x20, SCROLL 0x40, INFO 0x80, NUMERIC 0x100, ALPHA 0x200, OTHER 0x400 */
static int key_mask(int k) {
    if (k == 403) return 0x1;
    if (k == 404) return 0x2;
    if (k == 405) return 0x4;
    if (k == 406) return 0x8;
    if ((k >= 37 && k <= 40) || k == 13 || k == 461) return 0x10;
    if (k == 415 || k == 19 || k == 413 || k == 417 || k == 412 || k == 463) return 0x20;
    if (k == 457) return 0x80;
    if (k >= 48 && k <= 57) return 0x100;
    return 0x400;
}

static int wants(int k) { return ((S.hidden ? S.keyset & 0x1 : S.keyset) & key_mask(k)) != 0; }

/* quiet: preloaded or started by itself - no messages */
static int quiet(void) { return S.bg || S.autostart; }

/* ---------------- Image ---------------- */

static void show_layer(int on) {
    if (on == S.layer) return;
    S.layer = on;
    ui_set_layer(on ? webr_texture() : NULL);
}

static void on_frame(void) {
    if (!S.visible || S.bg) return;
    if (!S.layer && webr_texture()) {
        show_layer(1);
        osd_hide();
    } else if (S.layer) {
        ui_compose();
    }
}

/* ---------------- End ---------------- */

void hbbweb_close(const char *why) {
    if (!S.visible && !S.waiting) return;
    mt_log("[hbbweb] closed (%s)", why ? why : "-");
    int was_visible = S.visible && !S.bg;
    S.visible = S.waiting = S.bg = S.autostart = 0;
    S.gen++;
    S.pending_red = 0;
    S.red_replayed_at = 0;
    hbbmedia_stop_all();
    egress_set_channel(NULL);
    if (g_drm_used) {
        /* otherwise override/CustomData keep applying to the app */
        g_drm_used = 0;
        hbbmedia_drm(0);
        hbbdrm_reset();
        hbbdrm_unload();
    }
    show_layer(0);
    if (webr_running()) {
        webr_clear_user_scripts();
        webr_load("about:blank", NULL, NULL);
    }
    if (was_visible) {
        memset(S.vrect, 0, sizeof(S.vrect));
        act.video(0, 0, 0, 0);
        act.closed();
    }
}

/* started by itself and not visible yet (AIT/page on the way): the
 * keys, the wheel and long Red belong to TVF, as if nothing were running */
static int auto_unseen(void) { return S.autostart && !S.layer; }

int hbbweb_active(void) { return (S.visible || S.waiting) && !S.bg && !auto_unseen(); }

/* ---------------- Loading ---------------- */

/* Rewrite the entry point per broadcaster: [[regex, replacement]], first match wins,
 * $1..$9 in the replacement */
static void entry(const char *url, char *out, size_t n) {
    snprintf(out, n, "%s", url);
    cJSON *list = S.rewrites ? cJSON_Parse(S.rewrites) : NULL;
    const cJSON *r;
    cJSON_ArrayForEach(r, list) {
        const cJSON *re = cJSON_GetArrayItem(r, 0), *to = cJSON_GetArrayItem(r, 1);
        if (!cJSON_IsString(re) || !cJSON_IsString(to)) continue;
        regex_t rx;
        if (regcomp(&rx, re->valuestring, REG_EXTENDED) != 0) continue;
        regmatch_t m[10];
        if (regexec(&rx, url, 10, m, 0) == 0) {
            /* replace the match, the rest of the address stays */
            size_t o = 0;
            for (regoff_t i = 0; i < m[0].rm_so && o + 1 < n; i++) out[o++] = url[i];
            for (const char *p = to->valuestring; *p && o + 1 < n; p++) {
                if (p[0] == '$' && p[1] >= '1' && p[1] <= '9') {
                    regmatch_t g = m[p[1] - '0'];
                    for (regoff_t i = g.rm_so; g.rm_so >= 0 && i < g.rm_eo && o + 1 < n; i++) out[o++] = url[i];
                    p++;
                } else {
                    out[o++] = *p;
                }
            }
            for (const char *p = url + m[0].rm_eo; *p && o + 1 < n; p++) out[o++] = *p;
            out[o] = 0;
            regfree(&rx);
            mt_log("[hbbweb] entry %s -> %s", url, out);
            break;
        }
        regfree(&rx);
    }
    cJSON_Delete(list);
}

/* the app that Red starts: autostart (control 1), otherwise present (2) */
static const cJSON *pick(void) {
    const cJSON *a;
    for (int want = 1; want <= 2; want++) {
        cJSON_ArrayForEach(a, S.ait) {
            if ((int) jnum(a, "control", 0) == want && jstr(a, "url")) return a;
        }
    }
    return NULL;
}

/* User script: channel for the replacement, the replacement itself, and the
 * forwarding of its messages to TVF (it sends them to parent - in the
 * top window that is itself). Only those of the top window: the
 * replacement also runs in the iframes (consent, tracking pixels), whose "loaded
 * without keyset" would otherwise reset the app's keyset */
/* "de" -> "deu" (ISO 639-2/T, as HbbTV/OIPF expects), unknown -> "eng" */
static const char *iso639_2(const char *lang) {
    static const char *m[][2] = {{"de", "deu"}, {"en", "eng"}, {"fr", "fra"}, {"es", "spa"}, {"it", "ita"},
                                 {"nl", "nld"}, {"pl", "pol"}, {"pt", "por"}, {"tr", "tur"}, {"ru", "rus"},
                                 {"cs", "ces"}, {"hu", "hun"}, {"da", "dan"}, {"sv", "swe"}, {"fi", "fin"}};
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++)
        if (lang && !strcmp(lang, m[i][0])) return m[i][1];
    return "eng";
}

static char *user_script(void) {
    cJSON *fwd = cJSON_CreateObject();
    /* dash.js for the replacement's A/V objects: served by the relay
     * (main.c registers it there), otherwise the configured address */
    char dash[sizeof(cfg.dash)];
    if (relay_port()) snprintf(dash, sizeof(dash), "http://127.0.0.1:%d/f/hbbtv-dash.js", relay_port());
    else snprintf(dash, sizeof(dash), "%s", cfg.dash);
    if (dash[0]) cJSON_AddStringToObject(fwd, "dash", dash);
    if (g_hole) cJSON_AddTrueToObject(fwd, "hole"); /* media.js: hole via the tvf-hole attribute */
    if (g_native && g_eme) cJSON_AddTrueToObject(fwd, "eme"); /* drm.js: capabilities, Joyn */
    /* WebKit plays itself: media.js does not rewire anything globally, only A/V objects
     * after a PlayReady initiator of the DRM agent (drm.js) - only LG's pipeline
     * with the DRM client (hbbmedia.c) can handle those */
    if (g_native) cJSON_AddTrueToObject(fwd, "selective");
    /* preferred UI/audio/subtitle language of the OIPF configuration object
     * (ISO 639-2) follows TVF's UI language */
    cJSON_AddStringToObject(fwd, "lang", iso639_2(i18n_lang()));
    cJSON *ch = cJSON_AddObjectToObject(fwd, "channel");
    cJSON_AddStringToObject(ch, "name", S.name);
    cJSON_AddNumberToObject(ch, "number", S.channel);
    int onid, tsid, sid;
    if (act.dvb_ids && act.dvb_ids(&onid, &tsid, &sid)) {
        /* broadcaster apps pick their content by these (ARD Startleiste) */
        cJSON_AddNumberToObject(ch, "onid", onid);
        cJSON_AddNumberToObject(ch, "tsid", tsid);
        cJSON_AddNumberToObject(ch, "sid", sid);
        mt_log("[hbbweb] channel %d: onid %d tsid %d sid %d", S.channel, onid, tsid, sid);
    }
    /* audio/subtitle components with component tag (getComponents) */
    char *cj = act.components ? act.components() : NULL;
    cJSON *comps = cj ? cJSON_Parse(cj) : NULL;
    free(cj);
    if (cJSON_IsArray(comps)) {
        mt_log("[hbbweb] channel %d: %d component(s)", S.channel, cJSON_GetArraySize(comps));
        cJSON_AddItemToObject(ch, "components", comps);
    } else {
        cJSON_Delete(comps);
    }
    cJSON *ait = cJSON_AddArrayToObject(ch, "ait");
    const cJSON *a;
    cJSON_ArrayForEach(a, S.ait) {
        if (!jstr(a, "url")) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "org", jnum(a, "org", 0));
        cJSON_AddNumberToObject(o, "app", jnum(a, "app", 0));
        cJSON_AddStringToObject(o, "url", jstr(a, "url"));
        cJSON_AddItemToArray(ait, o);
    }
    char *fj = cJSON_PrintUnformatted(fwd);
    cJSON_Delete(fwd);
    static const char forward[] =
        "\n;(function(){if(window.top!==window)return;"
        "window.addEventListener('message',function(e){var m=e.data;if(e.source!==window)return;"
        "if(m&&typeof m.type==='string'&&m.type.indexOf('hbbtv-')===0){"
        "try{window.webkit.messageHandlers.tvf.postMessage(m);}catch(x){}}});})();\n";
    /* WebKit plays itself: media.js only in selective mode (see above); otherwise
     * the replacement's A/V objects use a real <video> with dash.js (MSE) */
    const char *media_js = g_media_js;
    size_t n = strlen(fj ? fj : "{}") + (g_polyfill ? strlen(g_polyfill) : 0) +
               (media_js ? strlen(media_js) : 0) + (g_drm_js ? strlen(g_drm_js) : 0) + sizeof(forward) + 64;
    char *s = malloc(n);
    if (s)
        snprintf(s, n, "window.__HBBTV_FWD = %s;\n%s\n%s%s\n%s", fj ? fj : "{}", media_js ? media_js : "", forward,
                 g_polyfill ? g_polyfill : "", g_drm_js ? g_drm_js : "");
    free(fj);
    return s;
}

static void load_app(const char *url) {
    entry(url, S.url, sizeof(S.url));
    /* until the page sets its keyset: all keys - when started by itself only
     * Red (like the red-button apps), otherwise a page without keyset would get
     * digits, OK and arrows from TVF */
    S.keyset = S.autostart ? 0x1 : 0x7FF;
    S.keyset_set = 0;
    S.hidden = 0;
    S.pointer = 0;
    S.ptr_x = S.ptr_y = -1;
    S.shown_at = 0;
    char *us = user_script();
    webr_clear_user_scripts();
    if (us) webr_user_script(us);
    free(us);
    /* Egress: the relay decides per host (egress.c: the channel's tvh tag,
     * then the setting - rules, direct or the fixed proxy); the fixed
     * proxy directly only without the relay */
    char px[200] = "";
    if (relay_port()) snprintf(px, sizeof(px), "http://127.0.0.1:%d", relay_port());
    else if (cfg.proxy[0]) snprintf(px, sizeof(px), "%s", cfg.proxy);
    webr_load(S.url, cfg.user_agent, px);
    webr_visible(1);
    mt_log("[hbbweb] loading %s (proxy %s)", S.url, px[0] ? px : "direct");
}

/* everything there (AIT, replacement, helper): load the app */
static void try_start(void) {
    if (!S.waiting || !S.ait_done || !webr_ready()) return;
    S.waiting = 0;
    const cJSON *app = pick();
    if (!app) {
        if (!quiet()) osd_message(S.ait_err ? _("HbbTV not reachable") : _("No HbbTV for this channel"), 3000, 0);
        mt_log("[hbbweb] no app (%s)%s", S.ait_err ? "AIT not available" : "AIT without autostart/present",
               S.bg ? " - preload" : "");
        S.gen++;
        S.bg = 0;
        return;
    }
    if (!g_polyfill) mt_log("[hbbweb] without OIPF replacement (%s missing)", cfg.polyfill);
    S.visible = 1;
    egress_set_channel(S.name); /* channel rule applies to page and videos from now on */
    /* the app gets the opening Red too (like a TV with it in the
     * background); when preloaded only with the Red that shows it; when started
     * by itself not at all (it shows its Red hint) */
    S.pending_red = !S.bg && !S.autostart;
    S.red_since = S.red_at = 0;
    S.red_replayed_at = 0;
    S.layer = 0;
    snprintf(S.app_name, sizeof(S.app_name), "%s", jstr(app, "name") ? jstr(app, "name") : S.name);
    if (!quiet()) {
        char msg[160];
        snprintf(msg, sizeof(msg), "HbbTV: %s", S.app_name);
        osd_message(msg, 2500, 0);
    }
    load_app(jstr(app, "url"));
}

static void on_ait(void *user, int status, const char *body, size_t len) {
    (void) len;
    if ((int) (intptr_t) user != S.gen || !S.waiting) return;
    cJSON_Delete(S.ait);
    S.ait = NULL;
    if (status == 404 && S.ait_tries < 16) {
        S.ait_retry_at = SDL_GetTicks() + 500; /* tvh sends the AIT shortly after the subscription starts */
        return;
    }
    cJSON *j = status == 200 && body ? cJSON_Parse(body) : NULL;
    const cJSON *apps = cJSON_GetObjectItemCaseSensitive(j, "apps");
    if (cJSON_IsArray(apps)) S.ait = cJSON_Duplicate(apps, 1);
    cJSON_Delete(j);
    S.ait_done = 1;
    S.ait_err = status != 200;
    mt_log("[hbbweb] AIT -> %d, %d apps", status, cJSON_GetArraySize(S.ait));
    try_start();
}

void hbbweb_prestart(void) {
    if (cfg.bundle[0]) webr_start(cfg.bundle, cfg.data_dir, PAGE_W, PAGE_H);
}

#define AIT_URL "local:ttx/hbbtv" /* AIT from the HTSP subscription (sidedata.c) */

static void red_once(void);
static void red_send(const char *why);
static int no_replay(const char *url);

/* show the preloaded app (Red): as on a TV, where it was already running in the background */
static void reveal(void) {
    S.bg = 0;
    S.autostart = 0;
    mt_log("[hbbweb] red on channel %d (%s): preloaded app %s", S.channel, S.name,
           S.visible ? "shown" : "still loading");
    if (!S.visible) return; /* AIT/page still on the way: try_start then shows it */
    char msg[160];
    snprintf(msg, sizeof(msg), "HbbTV: %s", S.app_name);
    osd_message(msg, 2500, 0);
    on_frame();
    S.red_since = S.red_at = 0;
    if (S.keyset_set && (S.keyset & 0x1) && no_replay(S.url)) {
        S.pending_red = 0; /* opens with its interface: showing it is enough */
    } else if (S.keyset_set && (S.keyset & 0x1)) {
        /* the app is long done: Red immediately, without waiting for a quiet keyset */
        red_send("to the preloaded app");
    } else {
        S.pending_red = 1; /* with hbbtv-loaded/keyset (red_once) */
    }
}

static int start(int channel, const char *name, const char *rewrites, int bg, int autostart) {
    if (!cfg.bundle[0]) return 0;
    hbbweb_close("new");
    if (webr_start(cfg.bundle, cfg.data_dir, PAGE_W, PAGE_H) != 0) {
        if (!bg && !autostart) osd_message(_("HbbTV renderer missing"), 3000, 1);
        return 1;
    }
    S.gen++;
    S.waiting = 1;
    S.bg = bg;
    S.autostart = autostart;
    S.channel = channel;
    snprintf(S.name, sizeof(S.name), "%s", name ? name : "");
    free(S.rewrites);
    S.rewrites = rewrites ? strdup(rewrites) : NULL;
    S.ait_done = S.ait_err = 0;
    S.ait_tries = 0;
    S.ait_retry_at = 0;
    fetch_get(AIT_URL, on_ait, (void *) (intptr_t) S.gen);
    mt_log("[hbbweb] %s on channel %d (%s)", bg ? "preload" : autostart ? "autostart" : "red", channel, S.name);
    return 1;
}

int hbbweb_open(int channel, const char *name, const char *rewrites) {
    if (S.bg && S.channel == channel && (S.visible || S.waiting)) {
        reveal();
        return 1;
    }
    return start(channel, name, rewrites, 0, 0);
}

void hbbweb_preload(int channel, const char *name, const char *rewrites) {
    if (S.visible || S.waiting) return; /* already running (visible or preloaded) */
    start(channel, name, rewrites, 1, 0);
}

void hbbweb_autostart(int channel, const char *name, const char *rewrites) {
    if (S.visible || S.waiting) return; /* already running (main.c does not start one before) */
    start(channel, name, rewrites, 0, 1);
}

int hbbweb_takes(int key) {
    if (!S.visible || S.bg || S.waiting || auto_unseen()) return 0;
    int vk = dom_code(key);
    return vk && wants(vk);
}

/* ---------------- Keys ---------------- */

int hbbweb_key(int key) {
    if (S.bg || auto_unseen()) return 0; /* preloaded or still invisible: everything belongs to TVF */
    if (S.waiting) {
        /* app still loading: Back/Exit cancel, otherwise swallow nothing */
        if (key == KEY_BACK || key == KEY_EXIT) {
            hbbweb_close("cancelled");
            return 1;
        }
        if (key == KEY_CH_UP || key == KEY_CH_DOWN) hbbweb_close("channel change");
        return 0;
    }
    if (!S.visible) return 0;
    if (key == KEY_EXIT) {
        hbbweb_close("Exit");
        return 1;
    }
    int vk = dom_code(key);
    /* a channel change closes HbbTV, the change itself belongs to TVF */
    if (vk == 427 || vk == 428) {
        hbbweb_close("channel change");
        return 0;
    }
    /* emergency exit: Back three times within 2 s (the first two go to the page as usual) */
    if (vk == 461) {
        uint32_t now = SDL_GetTicks();
        S.back_times[0] = S.back_times[1];
        S.back_times[1] = S.back_times[2];
        S.back_times[2] = now;
        if (S.back_times[0] && now - S.back_times[0] < 2000) {
            memset(S.back_times, 0, sizeof(S.back_times));
            hbbweb_close("3x Back");
            return 1;
        }
    }
    if (vk && wants(vk)) {
        S.pending_red = 0; /* user interaction has begun */
        S.autostart = 0;   /* from now on as if opened with Red */
        webr_key(vk);
        return 1;
    }
    /* Stop, or Back on a page without keyset: back to TV - not for
     * one that started by itself (it is only the Red hint; the key
     * belongs to TVF) */
    if (!S.autostart && (vk == 413 || (vk == 461 && !S.keyset_set))) {
        hbbweb_close(vk == 413 ? "Stop" : "Back without keyset");
        return 1;
    }
    return 0; /* not in the keyset: belongs to the TV */
}

/* Pointer (window coordinates): only to pages that announce pointer control
 * (keyset.supportsPointer, OIPF DAE 7.2.5.2) - as real mouse events.
 * Otherwise 0: then the click counts as OK (main.c) */
int hbbweb_pointer(int ev, int x, int y) {
    if (S.waiting || !S.visible || S.bg || !S.pointer) return 0;
    S.ptr_x = x * (double) PAGE_W / SCREEN_W;
    S.ptr_y = y * (double) PAGE_H / SCREEN_H;
    if (ev != PTR_MOVE) S.pending_red = S.autostart = 0; /* user interaction has begun */
    webr_pointer(ev == PTR_DOWN ? "down" : ev == PTR_UP ? "up" : "move", S.ptr_x, S.ptr_y);
    return 1;
}

/* Pointer hidden: end the page's hover */
void hbbweb_pointer_gone(void) {
    if (!S.visible || S.bg || !S.pointer || S.ptr_x < 0) return;
    S.ptr_x = S.ptr_y = -1;
    webr_pointer("leave", -1, -1);
}

/* Wheel with HbbTV open: never a channel change. Pages with pointer control
 * get it as a mouse wheel at the pointer position, otherwise it is up/down as in
 * TVF's lists - if the app has requested the arrow keys, otherwise nothing */
int hbbweb_wheel(int dir) {
    if (S.bg || auto_unseen()) return 0;
    if (S.waiting) return 1;
    if (!S.visible) return 0;
    if (S.pointer) {
        webr_scroll(dir, S.ptr_x >= 0 ? S.ptr_x : PAGE_W / 2.0, S.ptr_y >= 0 ? S.ptr_y : PAGE_H / 2.0);
        return 1;
    }
    int vk = dir > 0 ? 38 : 40;
    if (S.autostart && !wants(vk)) return 0; /* started by itself, without arrows: wheel zaps */
    if (wants(vk)) {
        S.pending_red = S.autostart = 0;
        webr_key(vk);
    }
    return 1;
}

/* Apps that ended on the replayed Red (they open with their interface): the
 * next start gets no Red. The keyset tells nothing - the ARD Startleiste
 * requests navigation (0x3F) while it waits hidden for Red, Anixe+ shows its
 * interface with the same keyset and closes on Red */
static char g_no_replay[NO_REPLAY_MAX][1024]; /* as S.url */
static int g_no_replay_n;

static int no_replay(const char *url) {
    for (int i = 0; i < g_no_replay_n && i < NO_REPLAY_MAX; i++)
        if (!strcmp(g_no_replay[i], url)) return 1;
    return 0;
}

/* the app ends/hides within RED_UNDO_MS of the replayed Red: remember it and
 * start it again without Red. 1 = done (the message is used up) */
static int red_undo(const char *what) {
    uint32_t now = SDL_GetTicks();
    if (!S.red_replayed_at || now - S.red_replayed_at > RED_UNDO_MS || !S.url[0]) return 0;
    S.red_replayed_at = 0;
    if (!no_replay(S.url)) snprintf(g_no_replay[g_no_replay_n++ % NO_REPLAY_MAX], sizeof(g_no_replay[0]), "%s", S.url);
    mt_log("[hbbweb] app %s right after the replayed red: started again without it", what);
    char url[sizeof(S.url)];
    snprintf(url, sizeof(url), "%s", S.url);
    S.pending_red = 0;
    S.red_at = 0;
    load_app(url);
    return 1;
}

/* the opening Red to the app (replayed, or to a preloaded app) */
static void red_send(const char *why) {
    S.pending_red = 0;
    S.red_at = 0;
    S.red_replayed_at = SDL_GetTicks();
    if (!S.red_replayed_at) S.red_replayed_at = 1;
    mt_log("[hbbweb] red %s", why);
    webr_key(403);
}

/* replay the opening Red once when the app is ready: its keyset
 * (with Red) has not changed for RED_SETTLE_MS, at most RED_MAX_MS */
static void red_once(void) {
    if (!S.pending_red || !(S.keyset & 0x1)) return;
    if (no_replay(S.url)) {
        mt_log("[hbbweb] red not replayed (the app opens with its interface)");
        S.pending_red = 0;
        S.red_at = 0;
        return;
    }
    uint32_t now = SDL_GetTicks();
    if (!S.red_since) S.red_since = now;
    uint32_t left = RED_MAX_MS - (now - S.red_since);
    if ((int32_t) left < 0) left = 0;
    S.red_at = now + (left < RED_SETTLE_MS ? left : RED_SETTLE_MS);
    if (!S.red_at) S.red_at = 1;
}

/* ---------------- Page messages (OIPF replacement) ---------------- */

static void launch_dvb(const char *url) {
    unsigned org = 0, id = 0;
    int n = 0;
    if (sscanf(url, "dvb://current.ait/%x.%x%n", &org, &id, &n) < 2) {
        mt_log("[hbbweb] %s: unknown", url);
        return;
    }
    const char *rest = url + n;
    const cJSON *a, *hit = NULL;
    cJSON_ArrayForEach(a, S.ait) {
        if ((unsigned) jnum(a, "org", -1) == org && (unsigned) jnum(a, "app", -1) == id && jstr(a, "url")) {
            hit = a;
            break;
        }
    }
    if (!hit) {
        mt_log("[hbbweb] %s not in AIT", url);
        return;
    }
    /* append query/fragment of the DVB address */
    char target[1024];
    const char *base = jstr(hit, "url");
    if (rest[0] == '?' && strchr(base, '?')) snprintf(target, sizeof(target), "%s&%s", base, rest + 1);
    else if (rest[0] == '?' && strchr(base, '#')) {
        const char *hash = strchr(base, '#');
        snprintf(target, sizeof(target), "%.*s%s%s", (int) (hash - base), base, rest, hash);
    } else {
        snprintf(target, sizeof(target), "%s%s", base, rest);
    }
    mt_log("[hbbweb] %s -> %s", url, target);
    load_app(target);
}

/* Position of a page element (1280x720 view) -> video window 1920x1080 */
static void video_from(const cJSON *r) {
    double vw = jnum(r, "vw", 0), x = jnum(r, "x", 0), y = jnum(r, "y", 0), w = jnum(r, "w", 0), h = jnum(r, "h", 0);
    if (vw <= 0 || w <= 0 || w >= vw * 0.95) video(0, 0, 0, 0);
    else {
        double k = 1920.0 / vw;
        video((int) (x * k + 0.5), (int) (y * k + 0.5), (int) (w * k + 0.5), (int) (h * k + 0.5));
    }
}

/* The page's <video>/<audio> (media.js) */
static void on_media(const cJSON *m) {
    const char *op = jstr(m, "op");
    int id = (int) jnum(m, "id", 0);
    if (!op || !S.visible) return;
    if (!strcmp(op, "play")) {
        const char *u = jstr(m, "url");
        mt_log("[hbbweb] media: element %d plays %s", id, u ? u : "-");
        if (u) hbbmedia_play(id, u, jstr(m, "mime"), jnum(m, "time", 0));
        video_from(cJSON_GetObjectItemCaseSensitive(m, "rect"));
    } else if (!strcmp(op, "resume")) {
        hbbmedia_resume(id);
    } else if (!strcmp(op, "pause")) {
        hbbmedia_pause(id);
    } else if (!strcmp(op, "seek")) {
        hbbmedia_seek(id, jnum(m, "time", 0));
    } else if (!strcmp(op, "stop")) {
        hbbmedia_stop(id);
    } else if (!strcmp(op, "rect")) {
        if (hbbmedia_active()) video_from(cJSON_GetObjectItemCaseSensitive(m, "rect"));
    }
}

/* Playback state to the page (hbbmedia -> media.js) */
void hbbweb_media_update(int id, const char *state, double time, double dur, int code) {
    if (!S.visible || !webr_running()) return;
    char js[256];
    int o = snprintf(js, sizeof(js), "window.__tvfMedia&&__tvfMedia.update(%d,{", id);
    if (state) o += snprintf(js + o, sizeof(js) - (size_t) o, "state:'%s',code:%d,", state, code);
    if (time >= 0) o += snprintf(js + o, sizeof(js) - (size_t) o, "time:%.3f,", time);
    if (dur > 0) o += snprintf(js + o, sizeof(js) - (size_t) o, "duration:%.3f,", dur);
    snprintf(js + o, sizeof(js) - (size_t) o, "})");
    webr_eval(js);
}

/* ---------------- DRM (drm.js, dash.js replacement) ---------------- */

typedef struct {
    int gen;
    char id[24];
} drm_ctx;

static void drm_result(void *user, int code, const char *result_msg) {
    drm_ctx *c = user;
    if (c->gen == S.gen && S.visible && webr_running()) {
        cJSON *a = cJSON_CreateString(c->id), *r = cJSON_CreateString(result_msg ? result_msg : "");
        char *as = cJSON_PrintUnformatted(a), *rs = cJSON_PrintUnformatted(r);
        size_t n = (as ? strlen(as) : 0) + (rs ? strlen(rs) : 0) + 96;
        char *js = malloc(n);
        if (js && as && rs) {
            snprintf(js, n, "window.__tvfDrm&&__tvfDrm.result(%s,%d,%s)", as, code, rs);
            webr_eval(js);
        }
        free(js);
        free(as);
        free(rs);
        cJSON_Delete(a);
        cJSON_Delete(r);
    }
    mt_log("[hbbweb] DRM message %s -> %d", c->id, code);
    free(c);
}

static void drm_rights(int state, const char *content_id) {
    mt_log("[hbbweb] DRM rights: state %d (%s)", state, content_id ? content_id : "");
    if (!S.visible || !webr_running()) return;
    cJSON *ci = cJSON_CreateString(content_id ? content_id : "");
    char *cs = cJSON_PrintUnformatted(ci);
    char js[512];
    snprintf(js, sizeof(js), "window.__tvfDrm&&__tvfDrm.rights(%d,%s,'')", state, cs ? cs : "''");
    webr_eval(js);
    free(cs);
    cJSON_Delete(ci);
}

/* The page's licence URL as an override on the DRM client (applies to the following
 * requests of the playback) */
static void drm_override(const char *la_url) {
    char x[2600];
    /* XML: escape & < > in the URL */
    char esc[2048];
    size_t o = 0;
    for (const char *p = la_url; *p && o + 6 < sizeof(esc); p++) {
        if (*p == '&') o += (size_t) snprintf(esc + o, sizeof(esc) - o, "&amp;");
        else if (*p == '<') o += (size_t) snprintf(esc + o, sizeof(esc) - o, "&lt;");
        else if (*p == '>') o += (size_t) snprintf(esc + o, sizeof(esc) - o, "&gt;");
        else esc[o++] = *p;
    }
    esc[o] = 0;
    snprintf(x, sizeof(x),
             "<PlayReadyInitiator xmlns=\"http://schemas.microsoft.com/DRM/2007/03/protocols/\">"
             "<LicenseServerUriOverride><LA_URL>%s</LA_URL></LicenseServerUriOverride></PlayReadyInitiator>",
             esc);
    hbbdrm_send(HBBDRM_INITIATOR, x, HBBDRM_PLAYREADY, NULL, NULL);
}

static void on_drm(const cJSON *m) {
    const char *op = jstr(m, "op");
    if (!op) return;
    g_drm_used = 1;
    hbbmedia_drm(1); /* subsequent playbacks with the PlayReady client */
    if (!strcmp(op, "send")) {
        drm_ctx *c = calloc(1, sizeof(*c));
        if (!c) return;
        c->gen = S.gen;
        snprintf(c->id, sizeof(c->id), "%s", jstr(m, "id") ? jstr(m, "id") : "");
        const char *msg = jstr(m, "msg");
        mt_log("[hbbweb] DRM agent: %s %.200s", jstr(m, "msgType") ? jstr(m, "msgType") : "", msg ? msg : "");
        hbbdrm_send(jstr(m, "msgType"), msg, jstr(m, "systemId"), drm_result, c);
    } else if (!strcmp(op, "protection")) {
        /* dash.js setProtectionData: {"com.microsoft.playready": {serverURL, httpRequestHeaders}} */
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(m, "data"), *pr = NULL, *k;
        cJSON_ArrayForEach(k, d) {
            if (k->string && strstr(k->string, "playready")) pr = k;
        }
        const char *url = pr ? (jstr(pr, "serverURL") ? jstr(pr, "serverURL") : jstr(pr, "laURL")) : NULL;
        const cJSON *hd = pr ? cJSON_GetObjectItemCaseSensitive(pr, "httpRequestHeaders") : NULL;
        mt_log("[hbbweb] page DRM data: PlayReady %s%s", url ? url : "-",
               cJSON_GetArraySize(hd) ? " (with headers - not supported)" : "");
        if (url) drm_override(url);
    }
}

static void on_msg(const cJSON *m) {
    const char *type = jstr(m, "type");
    if (!type || !S.visible) return;
    /* preloaded: picture and media stay with the TV until Red shows the app */
    if (S.bg && (!strcmp(type, "tvf-media") || !strcmp(type, "tvf-drm") || !strcmp(type, "hbbtv-launch") ||
                 !strcmp(type, "hbbtv-broadcast") || !strcmp(type, "hbbtv-video") ||
                 !strcmp(type, "hbbtv-component")))
        return;
    if (!strcmp(type, "tvf-media")) {
        on_media(m);
        return;
    }
    if (!strcmp(type, "tvf-drm")) {
        on_drm(m);
        return;
    }
    uint32_t now = SDL_GetTicks();
    if (!strcmp(type, "hbbtv-launch")) {
        /* app starts another one and exits by itself right after (RTL
         * Startleiste -> RTL-Text): the exit belongs to the switch */
        S.launch_at = now ? now : 1;
        const char *u = jstr(m, "url");
        if (u && !strncasecmp(u, "dvb:", 4)) launch_dvb(u);
        else if (u && (!strncasecmp(u, "http:", 5) || !strncasecmp(u, "https:", 6))) {
            /* app via URL = broadcast-independent (TS 102 796 6.2.2.6): the
             * device no longer shows a TV picture (e.g. a catch-up service from the Startleiste) */
            mt_log("[hbbweb] broadcast-independent app - no TV picture");
            if (act.broadcast) act.broadcast(0);
        }
    } else if (!strcmp(type, "hbbtv-broadcast")) {
        int on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "on"));
        mt_log("[hbbweb] page %s the TV picture (%s)", on ? "shows" : "stops", jstr(m, "why") ? jstr(m, "why") : "-");
        if (act.broadcast) act.broadcast(on);
    } else if (!strcmp(type, "hbbtv-component")) {
        int kind = (int) jnum(m, "kind", -1), index = (int) jnum(m, "index", -1);
        int on = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "on"));
        mt_log("[hbbweb] page %s component kind %d index %d%s%s", on ? "selects" : "unselects", kind, index,
               jstr(m, "sub") ? " sub " : "", jstr(m, "sub") ? jstr(m, "sub") : "");
        if (act.component) act.component(kind, index, jstr(m, "sub"), on);
    } else if (!strcmp(type, "hbbtv-streamevent")) {
        /* the page registers for a stream event (polyfill.js) - for the log */
        mt_log("[hbbweb] stream event '%s' of %s: %s (tag %d, event %d)", jstr(m, "name") ? jstr(m, "name") : "",
               jstr(m, "url") ? jstr(m, "url") : "-", jstr(m, "state") ? jstr(m, "state") : "-",
               (int) jnum(m, "tag", -1), (int) jnum(m, "id", -1));
    } else if (!strcmp(type, "hbbtv-exit")) {
        if (red_undo("exits")) return;
        if (!S.launch_at || now - S.launch_at > LAUNCH_EXIT_MS) hbbweb_close("app exits");
    } else if (!strcmp(type, "hbbtv-escape")) {
        hbbweb_close("3x Back in page");
    } else if (!strcmp(type, "hbbtv-video") && !hbbmedia_active()) {
        double vw = jnum(m, "vw", 0), x = jnum(m, "x", 0), y = jnum(m, "y", 0), w = jnum(m, "w", 0),
               h = jnum(m, "h", 0);
        if (vw <= 0 || w <= 0 || w >= vw * 0.95) video(0, 0, 0, 0);
        else {
            double k = 1920.0 / vw;
            video((int) (x * k + 0.5), (int) (y * k + 0.5), (int) (w * k + 0.5), (int) (h * k + 0.5));
        }
    } else if (!strcmp(type, "hbbtv-media")) {
        /* replacement's message; playback goes via tvf-media (media.js) */
        mt_log("[hbbweb] page %s", cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "playing")) ? "playing" : "stopped");
    } else if (!strcmp(type, "hbbtv-show")) {
        mt_log("[hbbweb] app shows itself");
        S.shown_at = now ? now : 1;
        S.hidden = 0;
    } else if (!strcmp(type, "hbbtv-hide")) {
        mt_log("[hbbweb] app hides itself");
        if (red_undo("hides itself")) return;
        /* "to TV" (ORF: hide(), only Red left in the keyset) - close as in the
         * overlay, Red restarts it */
        if (S.shown_at && now - S.shown_at > 1000) hbbweb_close("app hides itself");
    } else if (!strcmp(type, "hbbtv-keyset")) {
        S.keyset = (int) jnum(m, "value", S.keyset);
        S.keyset_set = 1;
        mt_log("[hbbweb] keyset 0x%X", S.keyset);
        red_once();
    } else if (!strcmp(type, "hbbtv-loaded")) {
        const cJSON *ks = cJSON_GetObjectItemCaseSensitive(m, "keyset");
        S.keyset_set = cJSON_IsNumber(ks);
        S.keyset = S.keyset_set ? (int) ks->valuedouble : S.autostart ? 0x1 : 0x7FF;
        S.pointer = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "pointer"));
        mt_log("[hbbweb] page with OIPF replacement %s, keyset %s0x%X", jstr(m, "url") ? jstr(m, "url") : "",
               S.keyset_set ? "" : "(default) ", S.keyset);
        if (S.keyset_set) red_once();
    } else if (!strcmp(type, "hbbtv-pointer")) {
        S.pointer = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(m, "on"));
        mt_log("[hbbweb] pointer control %s", S.pointer ? "on" : "off");
    } else if (!strcmp(type, "hbbtv-unload")) {
        S.keyset_set = 0;
        S.keyset = 0;
        S.pointer = 0;
    }
}

static void on_event(const cJSON *ev) {
    const char *e = jstr(ev, "event");
    if (!e) return;
    if (!strcmp(e, "ready") || !strcmp(e, "buffers")) {
        try_start();
    } else if (!strcmp(e, "msg")) {
        on_msg(cJSON_GetObjectItemCaseSensitive(ev, "data"));
    } else if (!strcmp(e, "console")) {
        mt_log("[web:js] %.300s", jstr(ev, "text") ? jstr(ev, "text") : "");
    } else if (!strcmp(e, "loaded")) {
        mt_log("[hbbweb] loaded %s", jstr(ev, "url") ? jstr(ev, "url") : "");
    } else if (!strcmp(e, "loadFailed")) {
        const char *err = jstr(ev, "error"), *u = jstr(ev, "url");
        mt_log("[hbbweb] load error %s: %s", u ? u : "", err ? err : "");
        /* the rejected first response of an HbbTV page (content type) is not an error */
        if (S.visible && !S.layer && u && !strcmp(u, S.url) && err && !strstr(err, "interrupted")) {
            if (!quiet()) osd_message(_("HbbTV page not reachable"), 3000, 1);
            hbbweb_close("load error");
        }
    } else if (!strcmp(e, "tlsError")) {
        mt_log("[hbbweb] TLS error %s (flags %d)", jstr(ev, "url") ? jstr(ev, "url") : "", (int) jnum(ev, "flags", 0));
        if (S.visible) {
            if (!quiet()) osd_message(_("HbbTV: invalid certificate"), 3000, 1);
            hbbweb_close("TLS");
        }
    } else if (!strcmp(e, "crashed")) {
        mt_log("[hbbweb] WebProcess gone (%s)", jstr(ev, "reason") ? jstr(ev, "reason") : "?");
        if (S.visible) {
            if (!quiet()) osd_message(_("HbbTV crashed"), 3000, 1);
            hbbweb_close("crash");
        }
    } else if (!strcmp(e, "exit")) {
        if (S.visible || S.waiting) {
            if (!quiet()) osd_message(_("HbbTV renderer stopped"), 3000, 1);
            hbbweb_close("helper exited");
        }
    }
}

/* Script from the package (res/hbbtv), NULL if it is missing */
static char *load_text(const char *path) {
    FILE *f = path && path[0] ? fopen(path, "rb") : NULL;
    if (!f) return NULL;
    char *t = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        t = n > 0 ? malloc((size_t) n + 1) : NULL;
        if (t && fread(t, 1, (size_t) n, f) == (size_t) n) t[n] = 0;
        else {
            free(t);
            t = NULL;
        }
    }
    fclose(f);
    return t;
}

void hbbweb_init(const hbbweb_cfg *c, const hbbweb_actions *a, void (*wake)(void)) {
    cfg = *c;
    act = *a;
    g_media_js = load_text(cfg.media_js);
    if (!g_media_js) mt_log("[hbbweb] %s missing - no media playback", cfg.media_js);
    g_drm_js = load_text(cfg.drm_js);
    if (!g_drm_js) mt_log("[hbbweb] %s missing - no DRM agent", cfg.drm_js);
    g_polyfill = load_text(cfg.polyfill);
    if (!g_polyfill) mt_log("[hbbweb] %s missing - no OIPF replacement", cfg.polyfill);
    /* Renderer capabilities, one per line in <bundle>/features:
     * video-hole = an element with the tvf-hole attribute and its own layer
     * punches a hole for the video plane in the compositor (WebKit patch 0008).
     * If it is missing (older bundle), media.js approximates the hole via clip-path. */
    if (cfg.bundle[0]) {
        char fp[300];
        snprintf(fp, sizeof(fp), "%s/features", cfg.bundle);
        FILE *f = fopen(fp, "r");
        if (f) {
            char line[64];
            while (fgets(line, sizeof(line), f)) {
                if (!strncmp(line, "video-hole", 10)) g_hole = 1;
                if (!strncmp(line, "media", 5) && (line[5] == '\n' || !line[5])) g_can_media = 1;
                if (!strncmp(line, "eme", 3) && (line[3] == '\n' || !line[3])) g_eme = 1;
            }
            fclose(f);
        }
    }
    hbbdrm_on_rights(drm_rights);
    static const webr_actions wa = {on_event, on_frame};
    webr_init(&wa, wake);
    g_native = g_can_media && cfg.media_sock[0];
    webr_set_media(g_native ? cfg.media_sock : NULL);
    mt_log("[hbbweb] renderer %s (video hole %s, media %s%s), proxy %s", cfg.bundle,
           g_hole ? "in compositor" : "via clip-path",
           g_native ? "in WebKit -> TV decoder" : g_can_media ? "via media.js (WebKit capable)" : "via media.js",
           g_native && g_eme ? ", EME via the TV's CDMs" : "", cfg.proxy[0] ? cfg.proxy : "direct");
}

void hbbweb_set_media(const char *sock) {
    snprintf(cfg.media_sock, sizeof(cfg.media_sock), "%s", sock ? sock : "");
    int native = g_can_media && cfg.media_sock[0];
    if (native == g_native) return;
    g_native = native;
    webr_set_media(g_native ? cfg.media_sock : NULL);
    mt_log("[hbbweb] media %s (from next renderer start)", g_native ? "in WebKit" : "via media.js");
    if (webr_running() && !S.visible) webr_stop();
}

/* HbbTV stream events of the channel (sidedata.c) to the page's listeners
 * (polyfill.js __tvfSE): also to an app preloaded in the background, it
 * follows the programme as a visible one does */
static void stream_events(void) {
    sidedata_stream_event ev;
    while (sidedata_stream_event_next(&ev)) {
        if (!S.visible) continue;
        char js[64 + 2 * sizeof(ev.data)];
        int o = snprintf(js, sizeof(js), "window.__tvfSE&&__tvfSE(%d,%d,%llu,'", ev.component_tag, ev.event_id, ev.npt);
        for (int i = 0; i < ev.len && o + 3 < (int) sizeof(js); i++) o += snprintf(js + o, sizeof(js) - (size_t) o, "%02x", ev.data[i]);
        snprintf(js + o, sizeof(js) - (size_t) o, "')");
        webr_eval(js);
    }
}

void hbbweb_poll(void) {
    webr_poll();
    stream_events();
    if (S.waiting && S.ait_retry_at && (int32_t) (SDL_GetTicks() - S.ait_retry_at) >= 0) {
        S.ait_retry_at = 0;
        S.ait_tries++;
        fetch_get(AIT_URL, on_ait, (void *) (intptr_t) S.gen);
    }
    if (S.visible && S.pending_red && S.red_at && SDL_GetTicks() >= S.red_at) red_send("replayed");
}
