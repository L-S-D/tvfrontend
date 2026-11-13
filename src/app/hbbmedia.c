/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE /* strcasestr */
#include "hbbmedia.h"

#include "cJSON.h"
#include "log.h"
#include "egress.h"
#include "hbbdrm.h"
#include "luna.h"
#include "player.h"
#include "relay.h"
#include "smp_api.h"

#include <SDL.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define LIVE_BACK_MS 1500 /* after the end, wait this long for the next playback */

static hbbmedia_actions act;
static void (*g_wake)(void);
static player *g_pl;
static int g_drm;          /* next loads with the PlayReady client (option.drm) */
static int g_transport_in_option; /* test aid: transport type inside option (old way) */

void hbbmedia_set_player(struct player *p) { g_pl = p; }
void hbbmedia_drm(int on) { g_drm = on; }
void hbbmedia_transport_in_option(int on) { g_transport_in_option = on; }

/* Luna reply, queued for the main thread */
typedef struct rmsg {
    struct rmsg *next;
    int gen;
    int kind; /* 0 load, 1 event (subscribe), 2 other */
    char text[];
} rmsg;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static rmsg *g_head, *g_tail;

static struct {
    int gen;            /* session (discards replies of old sessions) */
    int id;             /* element of the page, 0 = none */
    char media_id[128]; /* from com.webos.media/load */
    long acb;           /* webOS 4 (no exported window): ACB of the pipeline, 0 = none */
    luna_sub *sub;
    int loaded, playing, paused_req, ended;
    double start;       /* seek there after loadCompleted */
    double time, dur;
    uint32_t live_at;   /* live picture back at (SDL ticks), 0 = not scheduled */
    int live_off;       /* live picture switched off by us */
    /* HLS via our own player */
    int own;
    char own_url[2048];
    int own_state;      /* last reported: 0 nothing, 1 playing, 2 paused, 3 ended, 4 error */
    int own_played;     /* has already played (otherwise the TV's player on error) */
    double own_dur;
    uint32_t own_t_at;  /* last time report */
    /* DRM: load waits for the PlayReady client (hbbdrm loads asynchronously) */
    int drm_wait_id;
    char drm_wait_url[2048], drm_wait_mime[64];
    uint32_t drm_wait_until;
} S;

/* ---------------- Luna (foreign thread) ---------------- */

typedef struct {
    int gen, kind;
} rctx;

static void on_luna(const char *reply, void *user) {
    rctx *c = user;
    size_t n = reply ? strlen(reply) : 0;
    rmsg *m = malloc(sizeof(rmsg) + n + 1);
    if (m) {
        m->next = NULL;
        m->gen = c->gen;
        m->kind = c->kind;
        memcpy(m->text, reply ? reply : "", n + 1);
        pthread_mutex_lock(&g_lock);
        if (g_tail) g_tail->next = m;
        else g_head = m;
        g_tail = m;
        pthread_mutex_unlock(&g_lock);
        if (g_wake) g_wake();
    }
    if (c->kind != 1) free(c); /* subscription contexts stay (further replies) */
}

static void call(const char *method, const char *payload, int kind) {
    char uri[96];
    snprintf(uri, sizeof(uri), "luna://com.webos.media/%s", method);
    rctx *c = malloc(sizeof(*c));
    if (!c) return;
    c->gen = S.gen;
    c->kind = kind;
    if (kind == 1) {
        S.sub = luna_subscribe(uri, payload, on_luna, c);
        if (!S.sub) {
            free(c);
            mt_log("[media] %s failed", method);
        }
        return;
    }
    if (luna_call(uri, payload, on_luna, c) != 0) {
        free(c);
        mt_log("[media] %s failed", method);
    }
}

static void call_id(const char *method) {
    if (!S.media_id[0]) return;
    mt_log("[media] %s", method);
    char p[192];
    snprintf(p, sizeof(p), "{\"mediaId\":\"%s\"}", S.media_id);
    call(method, p, 2);
}

/* ---------------- Session ---------------- */

static void end_session(int unload) {
    if (S.own) {
        S.own = 0;
        if (g_pl) player_stop(g_pl);
    }
    if (S.sub) luna_unsubscribe(S.sub);
    S.sub = NULL;
    if (unload) call_id("unload");
    if (S.acb) {
        smp_acb_detach(S.acb);
        S.acb = 0;
    }
    S.gen++;
    S.media_id[0] = 0;
    S.loaded = S.playing = S.ended = 0;
}

static void live_back_later(void) {
    S.live_at = SDL_GetTicks() + LIVE_BACK_MS;
    if (!S.live_at) S.live_at = 1;
}

static const char *transport(const char *url, const char *mime) {
    /* test aid: transport type given directly */
    if (mime && (!strcmp(mime, "URI") || !strcmp(mime, "HLS") || !strcmp(mime, "MPEG-DASH") || !strcmp(mime, "MSIIS")))
        return mime;
    if (mime && !strcmp(mime, "UMS-HLS")) return "HLS";
    if ((mime && strstr(mime, "dash")) || strstr(url, ".mpd")) return "MPEG-DASH";
    if ((mime && strcasestr(mime, "mpegurl")) || strstr(url, ".m3u8")) return "HLS";
    return "URI";
}

static void ums_load(const char *url, const char *mime);

void hbbmedia_play(int id, const char *url, const char *mime, double start) {
    /* always a new session: this also invalidates a still unanswered load of the
     * previous one (its reply unloads the pipeline again right away) */
    end_session(1);
    S.drm_wait_id = 0;
    S.id = id;
    S.start = start > 0 ? start : 0;
    S.time = S.start;
    S.dur = -1;
    S.paused_req = 0;
    S.live_at = 0;
    if (!S.live_off) {
        S.live_off = 1;
        act.live(0); /* free the decoder */
    }
    if (!strcmp(transport(url, mime), "HLS") && g_pl && !(mime && !strcmp(mime, "UMS-HLS"))) {
        /* HLS: own fetching, TS into our own pipeline */
        S.own = 1;
        S.own_state = 0;
        S.own_played = 0;
        S.own_dur = 0;
        S.own_t_at = 0;
        snprintf(S.own_url, sizeof(S.own_url), "%s", url);
        mt_log("[media] HLS via own player: %s", url);
        player_play_hls(g_pl, url, S.start);
        return;
    }
    if (g_drm && !hbbdrm_client()) {
        /* DRM content, client not there yet: hold back the load for up to 3 s */
        hbbdrm_load();
        S.drm_wait_id = id;
        snprintf(S.drm_wait_url, sizeof(S.drm_wait_url), "%s", url);
        snprintf(S.drm_wait_mime, sizeof(S.drm_wait_mime), "%s", mime ? mime : "");
        S.drm_wait_until = SDL_GetTicks() + 3000;
        mt_log("[media] waiting for PlayReady client");
        return;
    }
    ums_load(url, mime);
}

static void ums_load(const char *url, const char *mime) {
    const char *win = act.window_id ? act.window_id() : "";
    /* the media server knows no proxy: if an egress is set for the host,
     * it fetches the video via the relay */
    char host[256] = "", px[256], rel[3000] = "";
    const char *hs = strstr(url, "://");
    if (hs) {
        const char *e = hs + 3;
        size_t n = strcspn(e, "/:?");
        snprintf(host, sizeof(host), "%.*s", (int) n, e);
    }
    if (host[0] && egress_for(host, px, sizeof(px))[0]) {
        relay_url(url, rel, sizeof(rel));
        if (rel[0]) mt_log("[media] via %s (relay)", px);
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "uri", rel[0] ? rel : url);
    cJSON_AddStringToObject(p, "type", "media");
    cJSON *pl = cJSON_AddObjectToObject(p, "payload");
    /* transport type next to option, like the TV's browser (Chromium/webOS):
     * placed inside option, the pipeline died with encrypted DASH */
    if (!g_transport_in_option) cJSON_AddStringToObject(pl, "mediaTransportType", transport(url, mime));
    cJSON *opt = cJSON_AddObjectToObject(pl, "option");
    cJSON_AddStringToObject(opt, "appId", "com.lsd.tvf");
    if (win && *win) cJSON_AddStringToObject(opt, "windowId", win);
    if (g_transport_in_option) cJSON_AddStringToObject(opt, "mediaTransportType", transport(url, mime));
    if (g_drm && hbbdrm_client()) {
        cJSON *d = cJSON_AddObjectToObject(opt, "drm");
        cJSON_AddStringToObject(d, "type", "playready");
        cJSON_AddStringToObject(d, "clientId", hbbdrm_client());
    }
    char *s = cJSON_PrintUnformatted(p);
    cJSON_Delete(p);
    if (!s) return;
    mt_log("[media] load %s", s);
    call("load", s, 0);
    free(s);
}

void hbbmedia_resume(int id) {
    if (id != S.id) return;
    S.paused_req = 0;
    if (S.own) {
        player_set_paused(g_pl, 0);
        return;
    }
    if (S.loaded) call_id("play");
}

void hbbmedia_pause(int id) {
    if (id != S.id) return;
    S.paused_req = 1;
    if (S.own) {
        player_set_paused(g_pl, 1);
        return;
    }
    if (S.loaded) call_id("pause");
}

void hbbmedia_seek(int id, double sec) {
    if (id != S.id) return;
    if (S.own) {
        /* VOD: restart from the segment with this time (live: ignored) */
        if (S.own_dur > 0) {
            player_play_hls(g_pl, S.own_url, sec);
            S.own_state = 0;
        }
        return;
    }
    if (!S.loaded) {
        S.start = sec;
        return;
    }
    char p[192];
    snprintf(p, sizeof(p), "{\"mediaId\":\"%s\",\"position\":%lld}", S.media_id, (long long) (sec * 1000));
    call("seek", p, 2);
}

void hbbmedia_stop(int id) {
    if (id != S.id || !S.id) return;
    S.drm_wait_id = 0;
    mt_log("[media] stop (element %d)", id);
    end_session(1);
    S.id = 0;
    if (S.live_off) live_back_later();
}

void hbbmedia_stop_all(void) {
    S.drm_wait_id = 0;
    if (S.media_id[0] || S.sub) end_session(1);
    S.id = 0;
    S.live_at = 0;
    if (S.live_off) {
        S.live_off = 0;
        act.live(1);
    }
}

int hbbmedia_active(void) { return S.media_id[0] != 0 || S.sub != NULL || S.own || S.drm_wait_id; }

/* HLS via our own player: state to the page */
static void own_poll(void) {
    if (!S.own || !g_pl) return;
    player_media m;
    player_media_get(g_pl, &m);
    double dur = m.dur_ms > 0 ? m.dur_ms / 1000.0 : -1;
    if (dur > 0 && dur != S.own_dur) {
        S.own_dur = dur;
        act.update(S.id, NULL, -1, dur, 0);
    }
    int st = m.error ? 4 : m.ended ? 3 : m.playing ? 1 : (m.active && S.paused_req) ? 2 : 0;
    if (st != S.own_state) {
        S.own_state = st;
        if (st == 1) {
            S.own_played = 1;
            act.update(S.id, "playing", m.pos_ms >= 0 ? m.pos_ms / 1000.0 : -1, -1, 0);
        } else if (st == 2) act.update(S.id, "paused", -1, -1, 0);
        else if (st == 4 && !S.own_played) {
            /* our own player cannot handle this stream (separate audio track,
             * fMP4, encryption, ...): try the TV's player once - it
             * hangs on some TS HLS, but our own player plays that kind */
            mt_log("[media] HLS: own player failed, falling back to the TV's");
            S.own = 0;
            player_stop(g_pl);
            ums_load(S.own_url, "UMS-HLS");
        } else if (st == 3 || st == 4) {
            int id = S.id;
            mt_log("[media] HLS %s", st == 3 ? "ended" : "error");
            S.own = 0; /* do not stop the player: the live picture takes it over */
            act.update(id, st == 3 ? "ended" : "error", -1, -1, st == 4 ? -1 : 0);
            live_back_later();
        }
    }
    uint32_t now = SDL_GetTicks();
    if (S.own && st == 1 && m.pos_ms >= 0 && now - S.own_t_at >= 500) {
        S.own_t_at = now;
        act.update(S.id, NULL, m.pos_ms / 1000.0, -1, 0);
    }
}

/* ---------------- Replies (main thread) ---------------- */

static double num_ms(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble / 1000.0 : -1;
}

/* webOS 4: the media server's videoInfo ({"width":..,"scanType":"progressive",
 * "pixelAspectRatio":"1:1",..}) in the form StarfishMediaAPIs reports it and
 * the LG browser passes on to ACB ({"context":..,"content":"movie","video":{..}}) */
static char *acb_video_json(const cJSON *vi, int *w, int *h) {
    const cJSON *jw = cJSON_GetObjectItemCaseSensitive(vi, "width");
    const cJSON *jh = cJSON_GetObjectItemCaseSensitive(vi, "height");
    if (!cJSON_IsNumber(jw) || !cJSON_IsNumber(jh) || jw->valuedouble <= 0 || jh->valuedouble <= 0) return NULL;
    *w = (int) jw->valuedouble;
    *h = (int) jh->valuedouble;
    const cJSON *fr = cJSON_GetObjectItemCaseSensitive(vi, "frameRate");
    const cJSON *br = cJSON_GetObjectItemCaseSensitive(vi, "bitRate");
    const cJSON *afd = cJSON_GetObjectItemCaseSensitive(vi, "afd");
    const cJSON *rot = cJSON_GetObjectItemCaseSensitive(vi, "rotation");
    const cJSON *scan = cJSON_GetObjectItemCaseSensitive(vi, "scanType");
    const cJSON *par = cJSON_GetObjectItemCaseSensitive(vi, "pixelAspectRatio");
    const cJSON *hdr = cJSON_GetObjectItemCaseSensitive(vi, "hdrType");
    int pa = 1, pb = 1;
    if (cJSON_IsString(par) && (sscanf(par->valuestring, "%d:%d", &pa, &pb) != 2 || pa <= 0 || pb <= 0)) pa = pb = 1;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "context", S.media_id);
    cJSON_AddStringToObject(o, "content", "movie");
    cJSON *v = cJSON_AddObjectToObject(o, "video");
    cJSON_AddNumberToObject(v, "frameRate", cJSON_IsNumber(fr) ? fr->valuedouble : 0);
    cJSON_AddStringToObject(v, "scanType",
                            cJSON_IsString(scan) && strcasestr(scan->valuestring, "interlace") ? "VIDEO_INTERLACED"
                                                                                                : "VIDEO_PROGRESSIVE");
    cJSON_AddNumberToObject(v, "width", *w);
    cJSON_AddNumberToObject(v, "height", *h);
    cJSON_AddNumberToObject(v, "bitRate", cJSON_IsNumber(br) ? br->valuedouble : 0);
    cJSON_AddBoolToObject(v, "adaptive", 1);
    cJSON_AddStringToObject(v, "path", "network");
    cJSON *p = cJSON_AddObjectToObject(v, "pixelAspectRatio");
    cJSON_AddNumberToObject(p, "width", pa);
    cJSON_AddNumberToObject(p, "height", pb);
    cJSON_AddNumberToObject(v, "afd", cJSON_IsNumber(afd) ? afd->valuedouble : 0);
    cJSON_AddNumberToObject(v, "rotation", cJSON_IsNumber(rot) ? rot->valuedouble : 0);
    cJSON *d = cJSON_AddObjectToObject(v, "data3D");
    cJSON_AddStringToObject(d, "originalPattern", "2d");
    cJSON_AddStringToObject(d, "currentPattern", "2d");
    cJSON_AddStringToObject(d, "typeLR", "LR");
    cJSON_AddStringToObject(v, "hdrType", cJSON_IsString(hdr) ? hdr->valuestring : "none");
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return s;
}

static void on_event(const cJSON *j) {
    const cJSON *e;
    if ((e = cJSON_GetObjectItemCaseSensitive(j, "loadCompleted"))) {
        mt_log("[media] loadCompleted");
        smp_acb_loaded(S.acb);
        if (S.start > 0) hbbmedia_seek(S.id, S.start);
    } else if ((e = cJSON_GetObjectItemCaseSensitive(j, "playing"))) {
        S.playing = 1;
        mt_log("[media] playing");
        smp_acb_loaded(S.acb); /* loadCompleted may have come before the subscription */
        smp_acb_playing(S.acb, true);
        act.update(S.id, "playing", -1, S.dur, 0);
    } else if ((e = cJSON_GetObjectItemCaseSensitive(j, "paused"))) {
        mt_log("[media] paused");
        S.playing = 0;
        smp_acb_loaded(S.acb);
        smp_acb_playing(S.acb, false);
        act.update(S.id, "paused", -1, -1, 0);
    } else if ((e = cJSON_GetObjectItemCaseSensitive(j, "videoInfo"))) {
        char *s = cJSON_PrintUnformatted(j);
        mt_log("[media] event %.200s", s ? s : "?");
        free(s);
        int w = 0, h = 0;
        char *v = S.acb ? acb_video_json(e, &w, &h) : NULL;
        if (v) smp_acb_video_info(S.acb, v, w, h);
        free(v);
    } else if ((e = cJSON_GetObjectItemCaseSensitive(j, "currentTime"))) {
        double t = num_ms(e, "currentTime");
        if (t >= 0) {
            S.time = t;
            act.update(S.id, NULL, t, -1, 0);
        }
    } else if ((e = cJSON_GetObjectItemCaseSensitive(j, "sourceInfo"))) {
        /* duration (ms) is at the top level or in programInfo, depending on firmware */
        double d = num_ms(e, "duration");
        const cJSON *pi = cJSON_GetObjectItemCaseSensitive(e, "programInfo");
        if (d < 0 && cJSON_IsArray(pi)) d = num_ms(cJSON_GetArrayItem(pi, 0), "duration");
        mt_log("[media] sourceInfo, duration %.1f s", d);
        if (d > 0) {
            S.dur = d;
            act.update(S.id, NULL, -1, d, 0);
        }
    } else if (cJSON_GetObjectItemCaseSensitive(j, "bufferRange") || cJSON_GetObjectItemCaseSensitive(j, "subscription")) {
        /* buffer level / subscription confirmation: nothing to do */
    } else if ((e = cJSON_GetObjectItemCaseSensitive(j, "bufferingStart"))) {
        act.update(S.id, "buffering", -1, -1, 0);
    } else if ((e = cJSON_GetObjectItemCaseSensitive(j, "bufferingEnd"))) {
        if (S.playing) act.update(S.id, "playing", -1, -1, 0);
    } else if ((e = cJSON_GetObjectItemCaseSensitive(j, "endOfStream"))) {
        mt_log("[media] endOfStream");
        int id = S.id;
        end_session(1);
        act.update(id, "ended", -1, -1, 0);
        live_back_later();
    } else if ((e = cJSON_GetObjectItemCaseSensitive(j, "error"))) {
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(e, "errorCode");
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(e, "errorText");
        int code = cJSON_IsNumber(c) ? (int) c->valuedouble : -1;
        mt_log("[media] error %d %s", code, cJSON_IsString(t) ? t->valuestring : "");
        int id = S.id;
        end_session(1);
        act.update(id, "error", -1, -1, code);
        live_back_later();
    } else {
        char *s = cJSON_PrintUnformatted(j);
        mt_log("[media] event %.200s", s ? s : "?");
        free(s);
    }
}

static void on_msg(rmsg *m) {
    if (m->gen != S.gen) {
        /* old session: if its load still went through, unload the pipeline right
         * away (otherwise it would keep running orphaned) */
        if (m->kind == 0) {
            cJSON *o = cJSON_Parse(m->text);
            const cJSON *mid = cJSON_GetObjectItemCaseSensitive(o, "mediaId");
            if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "returnValue")) && cJSON_IsString(mid)) {
                char p[192];
                snprintf(p, sizeof(p), "{\"mediaId\":\"%s\"}", mid->valuestring);
                mt_log("[media] unloading orphaned pipeline %s", mid->valuestring);
                call("unload", p, 2);
            }
            cJSON_Delete(o);
        }
        return;
    }
    cJSON *j = cJSON_Parse(m->text);
    if (!j) {
        mt_log("[media] reply not JSON: %.120s", m->text);
        return;
    }
    const cJSON *rv = cJSON_GetObjectItemCaseSensitive(j, "returnValue");
    if (m->kind == 0) { /* load */
        const cJSON *mid = cJSON_GetObjectItemCaseSensitive(j, "mediaId");
        if (cJSON_IsTrue(rv) && cJSON_IsString(mid)) {
            snprintf(S.media_id, sizeof(S.media_id), "%s", mid->valuestring);
            mt_log("[media] mediaId %s", S.media_id);
            /* webOS 4 (no exported window, see main.c): ACB binds the
             * pipeline to the video plane, as smp_api does for our own */
            const char *win = act.window_id ? act.window_id() : "";
            if (!(win && *win)) {
                S.acb = smp_acb_attach(S.media_id, -1);
                if (!S.acb) mt_log("[media] neither an exported window nor ACB: the picture stays invisible");
            }
            char p[192];
            snprintf(p, sizeof(p), "{\"mediaId\":\"%s\"}", S.media_id);
            call("subscribe", p, 1);
            /* play right away (like the system player): loadCompleted can come
             * before the subscription, without play the pipeline only prebuffers */
            S.loaded = 1;
            call_id(S.paused_req ? "pause" : "play");
            smp_acb_playing(S.acb, !S.paused_req);
        } else {
            mt_log("[media] load rejected: %.200s", m->text);
            int id = S.id;
            end_session(0);
            act.update(id, "error", -1, -1, -1);
            live_back_later();
        }
    } else if (m->kind == 1) {
        if (cJSON_IsFalse(rv)) mt_log("[media] subscribe rejected: %.200s", m->text);
        else on_event(j);
    } else if (cJSON_IsFalse(rv)) {
        mt_log("[media] call rejected: %.200s", m->text);
    }
    cJSON_Delete(j);
}

void hbbmedia_init(const hbbmedia_actions *a, void (*wake)(void)) {
    act = *a;
    g_wake = wake;
}

void hbbmedia_poll(void) {
    own_poll();
    if (S.drm_wait_id && (hbbdrm_client() || SDL_GetTicks() >= S.drm_wait_until)) {
        if (!hbbdrm_client()) mt_log("[media] PlayReady client missing - loading without DRM");
        S.drm_wait_id = 0;
        ums_load(S.drm_wait_url, S.drm_wait_mime);
    }
    for (;;) {
        pthread_mutex_lock(&g_lock);
        rmsg *m = g_head;
        if (m) {
            g_head = m->next;
            if (!g_head) g_tail = NULL;
        }
        pthread_mutex_unlock(&g_lock);
        if (!m) break;
        on_msg(m);
        free(m);
    }
    /* nothing new after the end: live picture back */
    if (S.live_at && SDL_GetTicks() >= S.live_at) {
        S.live_at = 0;
        if (S.live_off && !hbbmedia_active()) {
            S.live_off = 0;
            mt_log("[media] live video back");
            act.live(1);
        }
    }
}
