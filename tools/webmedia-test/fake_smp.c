/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Emulation of smp_api for the test bench (host): a "decoder" with its own
 * clock from Play() on, which accepts data up to 2 s ahead of the current frame
 * (otherwise BufferFull), reports FRAMEREADY with the frame's PTS every 40 ms,
 * ENDOFSTREAM at the end after pushEOS, and acknowledges Load/Unload like the real
 * pipeline. Every call ends up in the log ("[smp] ...").
 */
#include "smp_api.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MS 1000000LL

struct smp_api {
    pthread_mutex_t lock;
    pthread_t thread;
    int run, loaded, playing, eos, eos_sent;
    smp_event_fn cb;
    void *user;
    int64_t clock;        /* pipeline PTS of the frame (ns), -1 = none yet */
    int64_t last_tick;
    int64_t first_v, last_v, last_a; /* fed */
    int64_t ptd;
    unsigned long long fv, fa, full;
};

static void *clock_main(void *arg) {
    smp_api *a = arg;
    while (1) {
        usleep(40000);
        pthread_mutex_lock(&a->lock);
        if (!a->run) {
            pthread_mutex_unlock(&a->lock);
            return NULL;
        }
        int64_t now = mt_now_ns();
        if (a->playing && a->clock >= 0) a->clock += now - a->last_tick;
        a->last_tick = now;
        if (a->clock < 0 && a->first_v >= 0) a->clock = a->first_v; /* first frame (preroll) */
        int64_t end = a->last_v > a->last_a ? a->last_v : a->last_a;
        if (a->eos && a->clock >= end) a->clock = end;
        int64_t c = a->clock;
        int send_eos = a->eos && !a->eos_sent && c >= end;
        if (send_eos) a->eos_sent = 1;
        smp_event_fn cb = a->cb;
        void *u = a->user;
        int loaded = a->loaded;
        pthread_mutex_unlock(&a->lock);
        if (loaded && cb && c >= 0) cb(SMP_EV_FRAMEREADY, c, NULL, u);
        if (send_eos && cb) {
            mt_log("[smp] ENDOFSTREAM at %.3f s", c / 1e9);
            cb(SMP_EV_ENDOFSTREAM, 0, NULL, u);
        }
    }
}

smp_api *smp_api_create(void) {
    smp_api *a = calloc(1, sizeof(*a));
    pthread_mutex_init(&a->lock, NULL);
    a->clock = -1;
    a->first_v = a->last_v = a->last_a = -1;
    a->run = 1;
    a->last_tick = mt_now_ns();
    pthread_create(&a->thread, NULL, clock_main, a);
    mt_log("[smp] create %p", (void *) a);
    return a;
}

void smp_api_destroy(smp_api *a) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    a->run = 0;
    pthread_mutex_unlock(&a->lock);
    pthread_join(a->thread, NULL);
    mt_log("[smp] destroy %p (fed video %llu, audio %llu, full %llu times)", (void *) a, a->fv, a->fa, a->full);
    free(a);
}

bool smp_api_notify_foreground(smp_api *a) { (void) a; return true; }
const char *smp_api_media_id(smp_api *a) { (void) a; return "fake"; }

bool smp_api_load(smp_api *a, const char *payload, smp_event_fn cb, void *user) {
    const char *p = strstr(payload, "\"ptsToDecode\":");
    pthread_mutex_lock(&a->lock);
    a->cb = cb;
    a->user = user;
    a->loaded = 1;
    a->ptd = p ? atoll(p + 14) : 0;
    pthread_mutex_unlock(&a->lock);
    mt_log("[smp] load ptsToDecode %.3f s%s%s", a->ptd / 1e9, strstr(payload, "\"drm\"") ? " DRM " : "",
           strstr(payload, "\"svpVersion\":40") ? "svpVersion 40" : "");
    cb(SMP_EV_LOADCOMPLETED, 0, NULL, user);
    /* picture size like the TV's decoder (VIDEO_INFO after loading); here the
     * upper limit from adaptiveStreaming */
    const char *mw = strstr(payload, "\"maxWidth\":"), *mh = strstr(payload, "\"maxHeight\":");
    if (mw && mh) {
        char vi[160];
        snprintf(vi, sizeof(vi), "{\"video\":{\"width\":%d,\"height\":%d,\"frameRate\":25}}", atoi(mw + 11), atoi(mh + 12));
        cb(SMP_EV_STR_VIDEO_INFO, 0, vi, user);
    }
    return true;
}

bool smp_api_play(smp_api *a) {
    pthread_mutex_lock(&a->lock);
    a->playing = 1;
    a->last_tick = mt_now_ns();
    smp_event_fn cb = a->cb;
    void *u = a->user;
    pthread_mutex_unlock(&a->lock);
    mt_log("[smp] play");
    if (cb) cb(SMP_EV_PLAYING, 0, NULL, u);
    return true;
}

bool smp_api_pause(smp_api *a) {
    pthread_mutex_lock(&a->lock);
    a->playing = 0;
    smp_event_fn cb = a->cb;
    void *u = a->user;
    pthread_mutex_unlock(&a->lock);
    mt_log("[smp] pause");
    if (cb) cb(SMP_EV_PAUSED, 0, NULL, u);
    return true;
}

bool smp_api_seek(smp_api *a, const char *millis) { (void) a; (void) millis; return false; }

bool smp_api_unload(smp_api *a) {
    pthread_mutex_lock(&a->lock);
    a->loaded = 0;
    smp_event_fn cb = a->cb;
    void *u = a->user;
    a->cb = NULL;
    pthread_mutex_unlock(&a->lock);
    mt_log("[smp] unload");
    if (cb) cb(SMP_EV_UNLOADCOMPLETED, 0, NULL, u);
    return true;
}

bool smp_api_push_eos(smp_api *a) {
    pthread_mutex_lock(&a->lock);
    a->eos = 1;
    pthread_mutex_unlock(&a->lock);
    mt_log("[smp] pushEOS");
    return true;
}

bool smp_api_flush(smp_api *a) { (void) a; return false; }
bool smp_api_set_time_to_decode(smp_api *a, const char *p) { (void) a; (void) p; return false; }

smp_feed_result smp_api_feed(smp_api *a, const void *data, unsigned size, int64_t pts_ns, int es, char *reply,
                             unsigned reply_len) {
    pthread_mutex_lock(&a->lock);
    int64_t c = a->clock >= 0 ? a->clock : (a->first_v >= 0 ? a->first_v : pts_ns);
    if (pts_ns > c + 2000 * MS) { /* more than 2 s ahead of the frame: full */
        a->full++;
        pthread_mutex_unlock(&a->lock);
        if (reply && reply_len) reply[0] = 0;
        return SMP_FEED_FULL;
    }
    /* EME: data from the (emulated) CDM carries "FAKESVP!" + number of subsamples */
    if (size >= 12 && !memcmp((const char *) data + size - 12, "FAKESVP!", 8)) {
        static unsigned svp;
        unsigned nsubs;
        memcpy(&nsubs, (const char *) data + size - 4, 4);
        if (svp++ < 3)
            mt_log("[smp] SVP data %s %u bytes (%u subsamples)", es == SMP_ES_VIDEO ? "video" : "audio", size, nsubs);
    }
    if (es == SMP_ES_VIDEO) {
        if (a->first_v < 0) {
            a->first_v = pts_ns;
            mt_log("[smp] first frame pts %.3f s", pts_ns / 1e9);
        }
        if (pts_ns > a->last_v) a->last_v = pts_ns;
        a->fv++;
    } else {
        if (pts_ns > a->last_a) a->last_a = pts_ns;
        a->fa++;
    }
    pthread_mutex_unlock(&a->lock);
    if (reply && reply_len) reply[0] = 0;
    return SMP_FEED_OK;
}

int smp_api_render_queue_length(smp_api *a) { (void) a; return -1; }
int64_t smp_api_current_playtime(smp_api *a) {
    pthread_mutex_lock(&a->lock);
    int64_t c = a->clock;
    pthread_mutex_unlock(&a->lock);
    return c;
}

bool smp_api_acb_enable(const char *app_id, const smp_acb_opts *opts) { (void) app_id; (void) opts; return false; }
void smp_api_video_info(smp_api *a, const char *json, int width, int height) { (void) a; (void) json; (void) width; (void) height; }
void smp_api_set_rect(smp_api *a, int x, int y, int w, int h) { (void) a; (void) x; (void) y; (void) w; (void) h; }
