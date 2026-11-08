/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Media server for WebKit's own playback, see webmedia.h.
 *
 * Course of a session (both sinks of a WebKit pipeline, HELLO key):
 * as soon as the announced streams have their caps and data (video from a
 * keyframe on), the main thread creates the pipeline (live picture off first), the
 * feeder loads it (BUFFERSTREAM, PTS base: first keyframe at margin as in the
 * live player) and feeds the queues alternately by PTS. What the
 * decoder takes goes back as an ACK - only then does the sink send more
 * (back-pressure via credit, the connection itself is always read
 * immediately). Position from FRAMEREADY, end from ENDOFSTREAM after pushEOS.
 * FLUSH (seek): unload the pipeline and reload it from the data of the new
 * epoch.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* accept4, struct ucred */
#endif
#include "webmedia.h"

#include "cJSON.h"
#include "cdm.h"
#include "log.h"
#include "smp_api.h"
#include "webdrm.h"

#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define NO_TIME INT64_MIN
#define MS 1000000LL

enum { M_HELLO = 1, M_CAPS, M_DATA, M_FLUSH, M_EOS, M_STATE, M_RECT, M_VOLUME };
enum { T_WINDOW = 101, T_ACK, T_POS, T_EOSDONE, T_ERROR };

#define WINDOW_VIDEO (4u << 20)
#define WINDOW_AUDIO (512u << 10)
#define MARGIN_MS 1000   /* first keyframe at this PTS (like the live player) */
#define GRACE_MS 150     /* after the first HELLO, wait this long for the second stream */
#define ACK_STEP (64u << 10)

#define DATA_KEY 1
#define DATA_CRYPTO 4

typedef struct item {
    struct item *next;
    int64_t pts, dur;
    uint32_t flags, len;
    uint8_t *crypto; /* crypto block (DATA_CRYPTO), NULL = clear */
    uint32_t clen;
    uint8_t *svp;    /* from the CDM for the secure path (fed instead of data) */
    uint32_t svplen;
    uint8_t data[];
} item;

static void item_free(item *it) {
    free(it->crypto);
    free(it->svp);
    free(it);
}

struct wm_session;

typedef struct {
    struct wm_session *s;
    int gen;
    volatile int unloaded;
} cbctx;

typedef struct wm_stream {
    struct wm_session *sess;
    struct wm_stream *zombie; /* replaced streams of the session (freed only on teardown) */
    int fd;
    int video;
    pthread_mutex_t wlock; /* writing to fd */
    /* under sess->lock */
    char codec[8];
    int width, height, rate, channels, aot;
    double fps;
    int have_caps;
    uint32_t epoch;
    item *qh, *qt;
    size_t qbytes;
    uint64_t consumed, acked, frames;
    int eos, closed, key_seen;
    int64_t last_end, full_until;
    /* EME: system ID from the caps ("" = unknown), the stream is encrypted;
     * the last crypto info for its clear samples */
    char protection[48];
    int drm;
    int last_scheme;
    uint8_t last_kid[16];
    uint32_t last_kid_len;
    char last_sid[256];
} wm_stream;

typedef struct wm_session {
    struct wm_session *next;
    char key[96];
    pthread_mutex_t lock;
    pthread_cond_t cond;
    wm_stream *st[2]; /* [0] audio, [1] video */
    wm_stream *zombies;
    int readers;      /* reader threads still attached to the session */
    int playing;      /* WebKit is playing (STATE) */
    int64_t t_hello, t_flush;
    int gone, need_reload, failed;
    /* main thread */
    int req_api, req_release, finished;
    smp_api *api;
    cbctx *ctx;
    char window[128];
    int rect[4], rect_dirty;
    /* Feeder */
    pthread_t thread;
    int gen, loaded, play_issued, fed_any, eos_pushed, ended, loads;
    char drm_ks[48];   /* key system of the loaded pipeline, "" = without DRM */
    int drm_reload;    /* reload because of DRM: video from the next keyframe */
    int64_t drm_note;  /* last "waiting for CDM/keys" message */
    int64_t drm_wait_since;
    uint64_t drm_errors;
    int64_t base, eos_pushed_at;
    char loaded_codec[2][8];
    /* pipeline events */
    volatile int64_t frame_pts; /* PTS of the last shown frame (pipeline time), -1 = none */
    volatile int64_t frame_at;  /* when frame_pts last moved (ns), 0 = never yet */
    volatile int load_completed, got_eos;
} wm_session;

static struct {
    webmedia_actions act;
    void (*wake)(void);
    int view_w, lfd, live_taken;
    char name[64];
    char ctype[16];
    pthread_mutex_t lock; /* session list */
    wm_session *sessions;
    int64_t idle_since;
} W = {.lfd = -1, .lock = PTHREAD_MUTEX_INITIALIZER, .ctype = "LIVE"};

static int64_t now_ns(void) { return mt_now_ns(); }

static void wake(void) {
    if (W.wake) W.wake();
}

static void timed_wait(pthread_cond_t *c, pthread_mutex_t *m, int ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long) ms * 1000000L;
    ts.tv_sec += ts.tv_nsec / 1000000000L;
    ts.tv_nsec %= 1000000000L;
    pthread_cond_timedwait(c, m, &ts);
}

/* ------------------------------------------------------------------ */
/* Connection                                                          */
/* ------------------------------------------------------------------ */

static int read_all(int fd, void *buf, size_t n) {
    char *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return 0;
        p += r;
        n -= (size_t) r;
    }
    return 1;
}

static void send_msg(wm_stream *st, uint32_t type, const void *a, size_t an) {
    uint32_t h[2] = {type, (uint32_t) an};
    struct iovec iov[2] = {{h, sizeof(h)}, {(void *) a, an}};
    int n = 2;
    struct iovec *v = iov;
    pthread_mutex_lock(&st->wlock);
    while (n > 0) {
        struct msghdr m;
        memset(&m, 0, sizeof(m));
        m.msg_iov = v;
        m.msg_iovlen = (size_t) n;
        ssize_t r = sendmsg(st->fd, &m, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) break;
        while (n > 0 && (size_t) r >= v->iov_len) {
            r -= (ssize_t) v->iov_len;
            v++;
            n--;
        }
        if (n > 0) {
            v->iov_base = (char *) v->iov_base + r;
            v->iov_len -= (size_t) r;
        }
    }
    pthread_mutex_unlock(&st->wlock);
}

static void send_ack(wm_stream *st) {
    uint8_t b[16] = {0};
    memcpy(b, &st->epoch, 4);
    memcpy(b + 8, &st->consumed, 8);
    st->acked = st->consumed;
    send_msg(st, T_ACK, b, sizeof(b));
}

static void send_all_streams(wm_session *s, uint32_t type, const void *a, size_t an) {
    for (int i = 0; i < 2; i++)
        if (s->st[i] && !s->st[i]->closed) send_msg(s->st[i], type, a, an);
}

static void send_error(wm_session *s, const char *text) {
    mt_log("[webmedia] %s: error to page: %s", s->key, text);
    send_all_streams(s, T_ERROR, text, strlen(text));
}

static void drop_queue(wm_stream *st) {
    while (st->qh) {
        item *n = st->qh->next;
        item_free(st->qh);
        st->qh = n;
    }
    st->qt = NULL;
    st->qbytes = 0;
}

/* the stream does not exist (any more) or is closed */
static int open_stream(const wm_stream *st) { return st && !st->closed; }

static wm_session *find_session(const char *key) {
    for (wm_session *s = W.sessions; s; s = s->next)
        if (!s->finished && !s->gone && !strcmp(s->key, key)) return s;
    return NULL;
}

static void *feeder_main(void *arg);

/* HELLO: attach the stream to its session (new session = new feeder) */
static wm_stream *attach(int fd, const char *json) {
    cJSON *j = cJSON_Parse(json);
    const cJSON *k = cJSON_GetObjectItemCaseSensitive(j, "session");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(j, "stream");
    if (!cJSON_IsString(k) || !cJSON_IsString(t)) {
        cJSON_Delete(j);
        return NULL;
    }
    int video = !strcmp(t->valuestring, "video");
    pthread_mutex_lock(&W.lock);
    wm_session *s = find_session(k->valuestring);
    if (!s) {
        s = calloc(1, sizeof(*s));
        if (!s) {
            pthread_mutex_unlock(&W.lock);
            cJSON_Delete(j);
            return NULL;
        }
        snprintf(s->key, sizeof(s->key), "%s", k->valuestring);
        pthread_mutex_init(&s->lock, NULL);
        pthread_cond_init(&s->cond, NULL);
        s->t_hello = now_ns();
        s->frame_pts = -1;
        if (pthread_create(&s->thread, NULL, feeder_main, s) != 0) {
            free(s);
            pthread_mutex_unlock(&W.lock);
            cJSON_Delete(j);
            return NULL;
        }
        s->next = W.sessions;
        W.sessions = s;
        mt_log("[webmedia] new session %s", s->key);
    }
    wm_stream *st = calloc(1, sizeof(*st));
    if (st) {
        st->sess = s;
        st->fd = fd;
        st->video = video;
        st->last_end = NO_TIME;
        pthread_mutex_init(&st->wlock, NULL);
        pthread_mutex_lock(&s->lock);
        wm_stream *old = s->st[video];
        if (old) { /* same stream reconnected: the old one is done (freed on teardown) */
            old->closed = 1;
            drop_queue(old);
            shutdown(old->fd, SHUT_RDWR);
            old->zombie = s->zombies;
            s->zombies = old;
        }
        s->st[video] = st;
        s->readers++;
        if (s->loaded) s->need_reload = 1; /* stream joined after the load */
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        mt_log("[webmedia] %s: %s connected", s->key, video ? "video" : "audio");
        char w[48];
        int n = snprintf(w, sizeof(w), "{\"window\":%u}", video ? WINDOW_VIDEO : WINDOW_AUDIO);
        send_msg(st, T_WINDOW, w, (size_t) n);
    }
    pthread_mutex_unlock(&W.lock);
    cJSON_Delete(j);
    return st;
}

static double jnum(const cJSON *o, const char *k, double def) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static void on_caps(wm_stream *st, const char *json) {
    cJSON *j = cJSON_Parse(json);
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "codec");
    if (cJSON_IsString(c)) {
        wm_session *s = st->sess;
        if (st->have_caps && strcmp(st->codec, c->valuestring) && s->loaded) s->need_reload = 1;
        snprintf(st->codec, sizeof(st->codec), "%s", c->valuestring);
        st->width = (int) jnum(j, "width", 0);
        st->height = (int) jnum(j, "height", 0);
        st->fps = jnum(j, "fps", 0);
        st->rate = (int) jnum(j, "rate", 0);
        st->channels = (int) jnum(j, "channels", 0);
        st->aot = (int) jnum(j, "aot", 2);
        const cJSON *pr = cJSON_GetObjectItemCaseSensitive(j, "protection");
        snprintf(st->protection, sizeof(st->protection), "%s", cJSON_IsString(pr) ? pr->valuestring : "");
        if (cJSON_IsString(pr)) {
            if (!st->drm && s->loaded && !s->drm_ks[0]) s->need_reload = 1;
            st->drm = 1;
        }
        st->have_caps = 1;
        mt_log("[webmedia] %s: %s %s %dx%d %.2f fps %d Hz %d ch%s%s", s->key, st->video ? "video" : "audio", st->codec,
               st->width, st->height, st->fps, st->rate, st->channels, cJSON_IsString(pr) ? ", encrypted " : "",
               cJSON_IsString(pr) ? pr->valuestring : "");
    }
    cJSON_Delete(j);
}

/* Reader of a connection: accepts everything immediately (back-pressure only via ACK) */
static void *reader_main(void *arg) {
    int fd = (int) (intptr_t) arg;
    wm_stream *st = NULL;
    for (;;) {
        uint32_t h[2];
        if (!read_all(fd, h, sizeof(h)) || h[1] > (32u << 20)) break;
        uint32_t type = h[0], len = h[1];
        if (type == M_DATA && st && len >= 32) {
            uint8_t head[32];
            if (!read_all(fd, head, sizeof(head))) break;
            uint32_t flags, epoch, rest = len - 32, clen = 0;
            memcpy(&flags, head + 24, 4);
            memcpy(&epoch, head + 28, 4);
            /* encrypted: crypto block before the unit */
            uint8_t *crypto = NULL;
            if (flags & DATA_CRYPTO) {
                if (rest < 4 || !read_all(fd, &clen, 4) || clen > rest - 4 || clen > 65536) break;
                crypto = malloc(clen ? clen : 1);
                if (!crypto || !read_all(fd, crypto, clen)) {
                    free(crypto);
                    break;
                }
                rest -= 4 + clen;
            }
            item *it = malloc(sizeof(item) + rest);
            if (!it) {
                free(crypto);
                break;
            }
            memcpy(&it->pts, head, 8);
            memcpy(&it->dur, head + 16, 8);
            it->flags = flags;
            it->len = rest;
            it->crypto = crypto;
            it->clen = clen;
            it->svp = NULL;
            it->svplen = 0;
            it->next = NULL;
            if (!read_all(fd, it->data, it->len)) {
                item_free(it);
                break;
            }
            wm_session *s = st->sess;
            pthread_mutex_lock(&s->lock);
            int keep = epoch == st->epoch && it->pts != NO_TIME && !st->closed;
            /* video from a keyframe on */
            if (keep && st->video && !st->key_seen) keep = st->key_seen = (it->flags & DATA_KEY) != 0;
            if (keep) {
                if (it->crypto) {
                    st->drm = 1;
                    /* loaded without DRM (no hint in the caps): now reload with DRM */
                    if (s->loaded && !s->drm_ks[0] && !s->need_reload) {
                        s->need_reload = s->drm_reload = 1;
                        mt_log("[webmedia] %s: %s encrypted - new pipeline with DRM", s->key,
                               st->video ? "video" : "audio");
                    }
                }
                if (st->qt) st->qt->next = it;
                else st->qh = it;
                st->qt = it;
                st->qbytes += it->len;
                st->frames++;
                pthread_cond_broadcast(&s->cond);
            } else {
                /* discarded counts as taken, otherwise the sink waits forever */
                if (epoch == st->epoch) {
                    st->consumed += it->len;
                    send_ack(st);
                }
                item_free(it);
            }
            pthread_mutex_unlock(&s->lock);
            continue;
        }
        char *b = malloc((size_t) len + 1);
        if (!b) break;
        if (len && !read_all(fd, b, len)) {
            free(b);
            break;
        }
        b[len] = 0;
        if (!st) {
            if (type == M_HELLO && strstr(b, "\"drm\"")) { /* OpenCDM client (EME) */
                free(b);
                webdrm_serve(fd);
                return NULL;
            }
            if (type == M_HELLO) st = attach(fd, b);
            free(b);
            if (!st) break;
            continue;
        }
        wm_session *s = st->sess;
        int rect_changed = 0;
        pthread_mutex_lock(&s->lock);
        if (type == M_CAPS) {
            on_caps(st, b);
        } else if (type == M_FLUSH && len >= 4) {
            memcpy(&st->epoch, b, 4);
            drop_queue(st);
            st->consumed = st->acked = 0;
            st->eos = st->key_seen = 0;
            st->last_end = NO_TIME;
            s->need_reload = 1;
            s->t_flush = now_ns();
            mt_log("[webmedia] %s: %s FLUSH -> epoch %u", s->key, st->video ? "video" : "audio", st->epoch);
        } else if (type == M_EOS && len >= 4) {
            uint32_t e;
            memcpy(&e, b, 4);
            if (e == st->epoch) st->eos = 1;
            mt_log("[webmedia] %s: %s EOS after %llu units", s->key, st->video ? "video" : "audio",
                   (unsigned long long) st->frames);
        } else if (type == M_STATE) {
            s->playing = strstr(b, "true") != NULL;
        } else if (type == M_RECT && st->video) {
            cJSON *j = cJSON_Parse(b);
            if (j) {
                s->rect[0] = (int) jnum(j, "x", 0);
                s->rect[1] = (int) jnum(j, "y", 0);
                s->rect[2] = (int) jnum(j, "w", 0);
                s->rect[3] = (int) jnum(j, "h", 0);
                s->rect_dirty = rect_changed = 1;
                cJSON_Delete(j);
            }
        } else if (type == M_VOLUME) {
            mt_log("[webmedia] %s: volume %s", s->key, b);
        }
        pthread_cond_broadcast(&s->cond);
        pthread_mutex_unlock(&s->lock);
        free(b);
        if (rect_changed) wake();
    }
    if (!st) {
        close(fd);
        return NULL;
    }
    /* closed: the stream has ended; fd stays open until the session is torn down
     * (no reuse of the number while someone is still writing) */
    wm_session *s = st->sess;
    pthread_mutex_lock(&s->lock);
    if (s->st[st->video] == st && !st->closed) {
        st->closed = 1;
        drop_queue(st);
        mt_log("[webmedia] %s: %s disconnected", s->key, st->video ? "video" : "audio");
        if (!open_stream(s->st[0]) && !open_stream(s->st[1])) s->gone = 1;
    }
    s->readers--; /* touch nothing after this: the main thread cleans up */
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    return NULL;
}

static void *accept_main(void *arg) {
    (void) arg;
    for (;;) {
        int fd = accept4(W.lfd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            mt_log("[webmedia] accept: %s", strerror(errno));
            return NULL;
        }
        struct ucred cr;
        socklen_t cl = sizeof(cr);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &cl) != 0 || cr.uid != getuid()) {
            mt_log("[webmedia] foreign connection rejected");
            close(fd);
            continue;
        }
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &at, reader_main, (void *) (intptr_t) fd) != 0) close(fd);
        pthread_attr_destroy(&at);
    }
}

/* ------------------------------------------------------------------ */
/* Pipeline                                                            */
/* ------------------------------------------------------------------ */

static void event_cb(int type, int64_t num, const char *str, void *user) {
    cbctx *ctx = user;
    wm_session *s = ctx->s;
    if (type == SMP_EV_UNLOADCOMPLETED) {
        ctx->unloaded = 1;
        return;
    }
    if (ctx->gen != __atomic_load_n(&s->gen, __ATOMIC_ACQUIRE)) return;
    switch (type) {
    case SMP_EV_FRAMEREADY:
        if (num != s->frame_pts) s->frame_at = mt_now_ns();
        s->frame_pts = num;
        return;
    case SMP_EV_LOADCOMPLETED:
        s->load_completed = 1;
        mt_log("[webmedia] %s: LOADCOMPLETED", s->key);
        break;
    case SMP_EV_PLAYING:
        mt_log("[webmedia] %s: PLAYING", s->key);
        break;
    case SMP_EV_PAUSED:
        mt_log("[webmedia] %s: PAUSED", s->key);
        break;
    case SMP_EV_ENDOFSTREAM:
        s->got_eos = 1;
        mt_log("[webmedia] %s: ENDOFSTREAM", s->key);
        break;
    case SMP_EV_STR_VIDEO_INFO: {
        mt_log("[webmedia] %s: VIDEO_INFO %s", s->key, str ? str : "");
        /* picture size for the exported window (source): otherwise the size of the
         * last live picture would apply - with a different size (arte 768x432 after
         * a 720p channel) the crop is wrong. Changes with the quality level */
        const char *w = str ? strstr(str, "\"width\"") : NULL, *h = str ? strstr(str, "\"height\"") : NULL;
        if (w && h && W.act.source) {
            const char *wc = strchr(w, ':'), *hc = strchr(h, ':');
            int vw = wc ? atoi(wc + 1) : 0, vh = hc ? atoi(hc + 1) : 0;
            if (vw > 0 && vh > 0) W.act.source(vw, vh);
        }
        return;
    }
    case SMP_EV_INT_ERROR:
    case SMP_EV_STR_ERROR:
        mt_log("[webmedia] %s: PIPELINE ERROR %d %lld %s", s->key, type, (long long) num,
               type == SMP_EV_STR_ERROR && str ? str : "");
        if (num == 601) mt_log("[webmedia] 601 = decoder busy");
        return;
    default:
        return;
    }
    pthread_mutex_lock(&s->lock);
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static const char *starfish_audio(const wm_stream *a) {
    if (!strcmp(a->codec, "AAC")) return "AAC";
    if (!strcmp(a->codec, "AC3")) return "AC3";
    if (!strcmp(a->codec, "EAC3")) return "AC3 PLUS";
    return NULL; /* MPEG audio: the live player decodes it itself - not here yet */
}

static int build_payload(wm_session *s, char *out, size_t cap) {
    wm_stream *v = open_stream(s->st[1]) ? s->st[1] : NULL, *a = open_stream(s->st[0]) ? s->st[0] : NULL;
    const char *vname = v ? (!strcmp(v->codec, "H265") ? "H265" : !strcmp(v->codec, "H264") ? "H264" : NULL) : NULL;
    const char *aname = a ? starfish_audio(a) : NULL;
    if (v && !vname) return -1;
    if (!v && !aname) return -1;
    const char *appid = getenv("APPID");
    if (!appid || !*appid) appid = "com.lsd.tvf";
    int n = snprintf(out, cap, "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{\"appId\":\"%s\"", appid);
#define ADD(...) n += snprintf(out + n, n < (int) cap ? cap - (size_t) n : 0, __VA_ARGS__)
    if (s->window[0]) ADD(",\"windowId\":\"%s\"", s->window);
    ADD(",\"needAudio\":%s,\"queryPosition\":false,\"useDroppedFrameEvent\":true,\"seekMode\":\"keep-rate\"",
        aname ? "true" : "false");
    /* EME: the samples come from the CDM for the secure video path (like LG's
     * browser and Kodi: drm.type + svpVersion 40 from webOS 5 on) */
    const char *drm = s->drm_ks[0] ? cdm_drm_type(s->drm_ks) : NULL;
    if (drm) ADD(",\"drm\":{\"type\":\"%s\"}", drm);
    if (strcmp(W.ctype, "none"))
        ADD(",\"transmission\":{\"contentsType\":\"%s\",\"trickType\":\"client-side\"}", W.ctype);
    ADD(",\"externalStreamingInfo\":{\"contents\":{\"codec\":{");
    if (vname) ADD("\"video\":\"%s\"", vname);
    if (aname) ADD("%s\"audio\":\"%s\"", vname ? "," : "", aname);
    ADD("},\"esInfo\":{\"pauseAtDecodeTime\":true,\"seperatedPTS\":true,\"ptsToDecode\":%lld",
        (long long) MARGIN_MS * MS);
    if (v && v->fps > 0) ADD(",\"videoFpsValue\":%d,\"videoFpsScale\":1", (int) (v->fps + 0.5));
    ADD("},\"format\":\"RAW\",\"provider\":\"%s\"", appid);
    if (aname && !strcmp(a->codec, "AAC"))
        ADD(",\"aacInfo\":{\"channels\":%d,\"profile\":%d,\"format\":\"adts\",\"frequency\":%g}", a->channels,
            a->aot ? a->aot : 2, a->rate / 1000.0);
    if (aname && !strcmp(a->codec, "EAC3"))
        ADD(",\"ac3PlusInfo\":{\"channels\":%d,\"frequency\":%g}", a->channels, a->rate / 1000.0);
    ADD("},\"streamQualityInfo\":true,\"streamQualityInfoNonFlushable\":true,"
        "\"streamQualityInfoCorruptedFrame\":true,\"restartStreaming\":false");
    if (drm) ADD(",\"svpVersion\":40");
    ADD(",\"bufferingCtrInfo\":{\"preBufferByte\":0,\"bufferMinLevel\":0,\"bufferMaxLevel\":0,"
        "\"qBufferLevelVideo\":0,\"qBufferLevelAudio\":0,"
        "\"srcBufferLevelVideo\":{\"minimum\":0,\"maximum\":%d},\"srcBufferLevelAudio\":{\"minimum\":0,\"maximum\":%d}}",
        8 << 20, 2 << 20);
    ADD("}");
    int hevc = vname && !strcmp(vname, "H265");
    ADD(",\"adaptiveStreaming\":{\"adaptiveResolution\":true,\"maxWidth\":%d,\"maxHeight\":%d,\"maxFrameRate\":60}",
        hevc ? 3840 : 1920, hevc ? 2160 : 1080);
    ADD("}}]}");
#undef ADD
    if (aname && v) snprintf(s->loaded_codec[0], sizeof(s->loaded_codec[0]), "%s", a->codec);
    return n < (int) cap ? 0 : -1;
}

/* ready to load: all announced streams with caps and data (video from
 * a keyframe on), the second stream has had a moment to announce itself */
static int ready_to_load(wm_session *s) {
    int any = 0;
    for (int i = 0; i < 2; i++) {
        wm_stream *st = s->st[i];
        if (!open_stream(st)) continue;
        if (!st->have_caps || !st->qh) return 0;
        any = 1;
    }
    int64_t now = now_ns();
    /* wait briefly after a FLUSH: the second stream flushes a few ms later
     * and until then would still have data of the old epoch in its queue */
    return any && now - s->t_hello >= GRACE_MS * MS && now - s->t_flush >= GRACE_MS * MS;
}

/* Feeder side: get the pipeline from the main thread or hand it back (under s->lock) */
static int get_api(wm_session *s) {
    s->req_api = 1;
    pthread_mutex_unlock(&s->lock);
    wake();
    pthread_mutex_lock(&s->lock);
    while (s->req_api && !s->gone) timed_wait(&s->cond, &s->lock, 100);
    return s->api != NULL;
}

static void put_api(wm_session *s) {
    if (!s->api) return;
    s->req_release = 1;
    pthread_mutex_unlock(&s->lock);
    wake();
    pthread_mutex_lock(&s->lock);
    while (s->req_release) timed_wait(&s->cond, &s->lock, 100);
    s->loaded = s->play_issued = s->fed_any = s->eos_pushed = s->ended = 0;
    s->load_completed = s->got_eos = 0;
    s->frame_pts = -1;
    s->frame_at = 0;
}

static void send_pos(wm_session *s, int64_t pos) {
    send_all_streams(s, T_POS, &pos, sizeof(pos));
}

/* Stream time of the picture on screen */
static int64_t position(wm_session *s) {
    int64_t f = s->frame_pts;
    if (f < 0 && !open_stream(s->st[1]) && s->api) f = smp_api_current_playtime(s->api); /* audio only */
    return f < 0 ? NO_TIME : f - MARGIN_MS * MS + s->base;
}

/* ---------------- EME: samples for the secure path ---------------- */

typedef struct {
    cdm_sample cs;
    uint8_t iv[16], kid[16];
    uint32_t *subs;
    char sid[256];
} svp_job;

/* Crypto info of a unit (under s->lock); 0 = block broken */
static int svp_prepare(wm_stream *st, const item *it, svp_job *j) {
    memset(j, 0, sizeof(*j));
    if (!it->crypto) {
        /* clear sample of an encrypted stream (lead-in): also through the CDM,
         * the secure path takes everything in its own format. LG's Widevine wrapper
         * only accepts it as cenc with a zero IV and one fully clear subsample
         * (without IV or as "unencrypted": error 4, on the TV on 2 Nov) */
        j->subs = malloc(8);
        if (!j->subs) return 0;
        j->subs[0] = it->len;
        j->subs[1] = 0;
        j->cs.scheme = 1;
        memcpy(j->kid, st->last_kid, sizeof(j->kid));
        j->cs.kid = st->last_kid_len ? j->kid : NULL; /* otherwise cdm.cpp takes a usable one */
        j->cs.kid_len = st->last_kid_len;
        j->cs.iv = j->iv; /* zeros */
        j->cs.iv_len = 16;
        j->cs.subs = j->subs;
        j->cs.nsubs = 1;
        snprintf(j->sid, sizeof(j->sid), "%s", st->last_sid);
        return 1;
    }
    const uint8_t *p = it->crypto;
    if (it->clen < 16) return 0;
    uint32_t ivl = p[1], kidl = p[2], sidl = p[3], nsubs;
    memcpy(&j->cs.crypt, p + 4, 4);
    memcpy(&j->cs.skip, p + 8, 4);
    memcpy(&nsubs, p + 12, 4);
    if (ivl > 16 || kidl > 16 || nsubs > 4096 || it->clen < 16 + ivl + kidl + sidl + nsubs * 8) return 0;
    p += 16;
    memcpy(j->iv, p, ivl); /* 8-byte IV zero-padded to 16 (like Chromium) */
    p += ivl;
    memcpy(j->kid, p, kidl);
    p += kidl;
    snprintf(j->sid, sizeof(j->sid), "%.*s", (int) sidl, (const char *) p);
    p += sidl;
    j->subs = malloc(nsubs ? nsubs * 8 : 8);
    if (!j->subs) return 0;
    memcpy(j->subs, p, nsubs * 8);
    j->cs.scheme = it->crypto[0] == 2 ? 2 : 1;
    j->cs.kid = j->kid;
    j->cs.kid_len = kidl;
    j->cs.iv = j->iv;
    j->cs.iv_len = 16;
    j->cs.subs = j->subs;
    j->cs.nsubs = nsubs;
    st->last_scheme = j->cs.scheme;
    memcpy(st->last_kid, j->kid, sizeof(st->last_kid));
    st->last_kid_len = kidl;
    snprintf(st->last_sid, sizeof(st->last_sid), "%s", j->sid);
    return 1;
}

enum { SVP_OK, SVP_WAIT, SVP_FAIL };

/* Unit -> the CDM's SVP data (without s->lock) */
static int svp_run(const char *drm_ks, int video, svp_job *j, item *it, char *why, size_t wn) {
    tvf_cdm *c = j->sid[0] ? cdm_find_session(j->sid) : NULL;
    if (!c && !it->crypto) c = cdm_find_system(drm_ks);
    if (!c) {
        if (it->crypto) snprintf(why, wn, "session %.100s missing", j->sid);
        else snprintf(why, wn, "no CDM for %s", drm_ks);
        return SVP_WAIT;
    }
    uint8_t *out = NULL;
    uint32_t n = 0;
    int rc = cdm_decrypt(c, video, &j->cs, it->data, it->len, &out, &n);
    cdm_unref(c);
    if (rc == 2) {
        snprintf(why, wn, "no key");
        return SVP_WAIT;
    }
    if (rc) {
        snprintf(why, wn, "Decrypt -> %d", rc);
        return SVP_FAIL;
    }
    it->svp = out;
    it->svplen = n;
    return SVP_OK;
}

/* Unit back to the head of the queue (under s->lock) */
static void requeue(wm_stream *st, item *it, int64_t retry_ms) {
    it->next = st->qh;
    st->qh = it;
    if (!st->qt) st->qt = it;
    st->qbytes += it->len;
    st->full_until = now_ns() + retry_ms * MS;
}

/* one unit into the decoder: 1 = taken (or discarded), 0 = full */
static int feed_one(wm_session *s, wm_stream *st) {
    item *it = st->qh;
    st->qh = it->next;
    if (!st->qh) st->qt = NULL;
    st->qbytes -= it->len;
    int64_t pts = it->pts - s->base + MARGIN_MS * MS;
    smp_api *api = s->api;
    int video = st->video;
    smp_feed_result r = SMP_FEED_OK;
    char reply[160] = "";
    int drop = !video && pts < MARGIN_MS * MS; /* audio before the first frame: discard */
    /* DRM pipeline: samples of encrypted streams go through the CDM */
    svp_job job;
    int cdm = !drop && s->drm_ks[0] && st->drm && !it->svp;
    int prep = cdm ? svp_prepare(st, it, &job) : 1;
    char ks[48];
    snprintf(ks, sizeof(ks), "%s", s->drm_ks);
    pthread_mutex_unlock(&s->lock);
    int sv = SVP_OK;
    char why[192] = "";
    if (cdm) {
        sv = prep ? svp_run(ks, video, &job, it, why, sizeof(why)) : SVP_FAIL;
        if (!prep) snprintf(why, sizeof(why), "crypto block broken");
        free(job.subs);
    }
    if (sv == SVP_OK && !drop)
        r = smp_api_feed(api, it->svp ? it->svp : it->data, it->svp ? it->svplen : it->len, pts,
                         video ? SMP_ES_VIDEO : SMP_ES_AUDIO, reply, sizeof(reply));
    pthread_mutex_lock(&s->lock);
    int64_t now = now_ns();
    if (sv == SVP_WAIT) {
        /* session/key not there yet (the page is fetching the licence) */
        if (st->closed) {
            item_free(it);
            return 1;
        }
        if (!s->drm_wait_since) s->drm_wait_since = now;
        if (now - s->drm_note > 2000 * MS) {
            s->drm_note = now;
            mt_log("[webmedia] %s: %s waiting (%s)", s->key, video ? "video" : "audio", why);
        }
        if (now - s->drm_wait_since > 20000 * MS) {
            send_error(s, "DRM: no key");
            s->failed = 1;
        }
        requeue(st, it, 50);
        return 0;
    }
    s->drm_wait_since = 0;
    if (sv == SVP_FAIL && s->drm_errors++ % 100 == 0)
        mt_log("[webmedia] %s: %s dropped: %s (%llu)", s->key, video ? "video" : "audio", why,
               (unsigned long long) s->drm_errors);
    if (r == SMP_FEED_FULL || r == SMP_FEED_PENDING) {
        if (st->closed) { /* closed in the meantime */
            item_free(it);
            return 1;
        }
        requeue(st, it, 10); /* with the SVP data: not through the CDM again */
        return 0;
    }
    if (r == SMP_FEED_ERROR) {
        mt_log("[webmedia] %s: Feed(%s) error: %s", s->key, video ? "V" : "A", reply);
        if (strstr(reply, "Unloading")) s->failed = 1;
    }
    s->fed_any = 1;
    int64_t end = it->pts + (it->dur > 0 ? it->dur : 0);
    if (st->last_end == NO_TIME || end > st->last_end) st->last_end = end;
    st->consumed += it->len;
    if (st->consumed - st->acked >= ACK_STEP || !st->qh) send_ack(st);
    item_free(it);
    return 1;
}

/* feed until the session ends or has to be reloaded (under s->lock) */
static void feed_loop(wm_session *s) {
    int64_t next_pos = 0;
    while (!s->gone && !s->need_reload && !s->failed) {
        int64_t now = now_ns();
        /* play/pause like WebKit */
        if (s->playing && !s->play_issued && (s->load_completed || s->fed_any)) {
            pthread_mutex_unlock(&s->lock);
            int ok = smp_api_play(s->api);
            pthread_mutex_lock(&s->lock);
            if (ok) s->play_issued = 1;
        } else if (!s->playing && s->play_issued) {
            pthread_mutex_unlock(&s->lock);
            smp_api_pause(s->api);
            pthread_mutex_lock(&s->lock);
            s->play_issued = 0;
        }
        /* next unit: the stream with the smallest PTS that is not full */
        wm_stream *pick = NULL;
        for (int i = 0; i < 2; i++) {
            wm_stream *st = s->st[i];
            if (!open_stream(st) || !st->qh || st->full_until > now) continue;
            if (!pick || st->qh->pts < pick->qh->pts) pick = st;
        }
        if (pick && feed_one(s, pick)) continue;
        /* end: all streams finished and fed -> pushEOS, then ENDOFSTREAM */
        int all_eos = 1, any = 0;
        int64_t end = NO_TIME;
        for (int i = 0; i < 2; i++) {
            wm_stream *st = s->st[i];
            if (!open_stream(st)) continue;
            any = 1;
            if (!st->eos || st->qh) all_eos = 0;
            if (st->last_end != NO_TIME && (end == NO_TIME || st->last_end > end)) end = st->last_end;
        }
        if (any && all_eos && !s->eos_pushed) {
            pthread_mutex_unlock(&s->lock);
            smp_api_push_eos(s->api);
            pthread_mutex_lock(&s->lock);
            s->eos_pushed = 1;
            s->eos_pushed_at = now;
            mt_log("[webmedia] %s: pushEOS (end at %.3f s)", s->key, end / 1e9);
        }
        int64_t pos = position(s);
        /* end: the pipeline's ENDOFSTREAM. Fallback in case it sends none:
         * while playing no new frame has come for 2 s and it is at the end
         * (the decoder often takes everything at once - pushEOS then comes long
         * before the end, that does not count) */
        int stalled = s->playing && s->play_issued && s->frame_at && now - s->frame_at > 2000 * MS &&
                      now - s->eos_pushed_at > 2000 * MS;
        int at_end = pos != NO_TIME && end != NO_TIME && pos >= end - 1500 * MS;
        if (s->eos_pushed && !s->ended && (s->got_eos || (stalled && at_end))) {
            s->ended = 1;
            if (end != NO_TIME) pos = end;
            send_pos(s, pos);
            for (int i = 0; i < 2; i++)
                if (open_stream(s->st[i])) send_msg(s->st[i], T_EOSDONE, &s->st[i]->epoch, 4);
            mt_log("[webmedia] %s: end at %.3f s%s", s->key, pos / 1e9, s->got_eos ? "" : " (without ENDOFSTREAM)");
        }
        if (now >= next_pos && pos != NO_TIME) {
            if (s->ended && end != NO_TIME) pos = end;
            send_pos(s, pos);
            next_pos = now + 200 * MS;
        }
        timed_wait(&s->cond, &s->lock, 10);
    }
}

static void *feeder_main(void *arg) {
    wm_session *s = arg;
    pthread_mutex_lock(&s->lock);
    while (!s->gone) {
        if (!ready_to_load(s)) {
            timed_wait(&s->cond, &s->lock, 20);
            continue;
        }
        if (!s->api && !get_api(s)) continue;
        /* during get_api the lock was free: a FLUSH may have emptied the queues
         * (2 Nov: ORF seek while the live picture was being handed over,
         * v->qh == NULL -> crash) - so check again */
        if (!ready_to_load(s)) continue;
        /* DRM: an encrypted stream (hint in the caps or already
         * encrypted samples) -> pipeline for the secure path */
        s->drm_ks[0] = 0;
        for (int i = 0; i < 2 && !s->drm_ks[0]; i++) {
            wm_stream *x = open_stream(s->st[i]) ? s->st[i] : NULL;
            if (!x || !x->drm) continue;
            const char *ks = cdm_key_system_for_uuid(x->protection);
            if (ks) {
                snprintf(s->drm_ks, sizeof(s->drm_ks), "%s", ks);
                break;
            }
            for (item *q = x->qh; q; q = q->next) {
                svp_job j;
                if (!q->crypto || !svp_prepare(x, q, &j)) continue;
                free(j.subs);
                tvf_cdm *c = cdm_find_session(j.sid);
                if (c) {
                    snprintf(s->drm_ks, sizeof(s->drm_ks), "%s", cdm_key_system(c));
                    cdm_unref(c);
                }
                break;
            }
        }
        /* after a switch to DRM: video from the next keyframe */
        wm_stream *v = open_stream(s->st[1]) ? s->st[1] : NULL;
        if (s->drm_reload && v) {
            while (v->qh && !(v->qh->flags & DATA_KEY)) {
                item *q = v->qh;
                v->qh = q->next;
                if (!v->qh) v->qt = NULL;
                v->qbytes -= q->len;
                v->consumed += q->len;
                item_free(q);
            }
            send_ack(v);
            if (!v->qh) { /* wait for the keyframe */
                timed_wait(&s->cond, &s->lock, 20);
                continue;
            }
        }
        s->drm_reload = 0;
        /* base: the first keyframe (or first audio) is at margin */
        s->base = v ? v->qh->pts : s->st[0]->qh->pts;
        s->need_reload = 0;
        char payload[4096];
        if (build_payload(s, payload, sizeof(payload)) != 0) {
            send_error(s, "format not supported");
            s->failed = 1;
            while (!s->gone && !s->need_reload) timed_wait(&s->cond, &s->lock, 100);
            s->failed = 0;
            continue;
        }
        cbctx *ctx = calloc(1, sizeof(*ctx));
        ctx->s = s;
        ctx->gen = __atomic_add_fetch(&s->gen, 1, __ATOMIC_RELEASE);
        free(s->ctx);
        s->ctx = ctx;
        smp_api *api = s->api;
        s->loads++;
        pthread_mutex_unlock(&s->lock);
        mt_log("[webmedia] %s: Load #%d from %.3f s: %s", s->key, s->loads, s->base / 1e9, payload);
        int64_t t = now_ns();
        int ok = smp_api_load(api, payload, event_cb, ctx);
        mt_log("[webmedia] %s: Load() -> %s after %.1f ms", s->key, ok ? "ok" : "FAILED", (now_ns() - t) / 1e6);
        pthread_mutex_lock(&s->lock);
        if (!ok) {
            send_error(s, "decoder not available");
            put_api(s);
            while (!s->gone && !s->need_reload) timed_wait(&s->cond, &s->lock, 100);
            continue;
        }
        s->loaded = 1;
        feed_loop(s);
        if (s->failed) send_error(s, "decoder unloaded");
        put_api(s);
        s->failed = 0;
    }
    if (s->api) put_api(s);
    s->finished = 1;
    pthread_mutex_unlock(&s->lock);
    mt_log("[webmedia] session %s ended", s->key);
    wake();
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Main thread                                                         */
/* ------------------------------------------------------------------ */

static void release_api(wm_session *s) {
    pthread_mutex_lock(&s->lock);
    smp_api *api = s->api;
    cbctx *ctx = s->ctx;
    int loaded = s->loaded;
    s->ctx = NULL;
    pthread_mutex_unlock(&s->lock);
    if (api) {
        int64_t t = now_ns();
        if (loaded) {
            smp_api_unload(api);
            while (ctx && !ctx->unloaded && now_ns() - t < 1000 * MS) usleep(5000);
        }
        smp_api_destroy(api);
        mt_log("[webmedia] %s: pipeline torn down after %.1f ms%s", s->key, (now_ns() - t) / 1e6,
               loaded && ctx && !ctx->unloaded ? " (without UNLOADCOMPLETED)" : "");
    }
    free(ctx);
    pthread_mutex_lock(&s->lock);
    s->api = NULL;
    s->req_release = 0;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
}

static void create_api(wm_session *s) {
    /* the decoder now belongs to the page: live picture off (unloads synchronously) */
    W.act.live(0);
    W.live_taken = 1;
    W.idle_since = 0;
    smp_api *api = smp_api_create();
    if (api) smp_api_notify_foreground(api);
    const char *win = W.act.window_id ? W.act.window_id() : "";
    pthread_mutex_lock(&s->lock);
    snprintf(s->window, sizeof(s->window), "%s", win ? win : "");
    s->api = api;
    s->req_api = 0;
    pthread_cond_broadcast(&s->cond);
    pthread_mutex_unlock(&s->lock);
    if (!api) mt_log("[webmedia] %s: no pipeline", s->key);
}

static void apply_rect(const int r[4]) {
    if (W.view_w <= 0 || r[2] <= 0 || r[3] <= 0 || r[2] >= W.view_w * 0.95) {
        W.act.video(0, 0, 0, 0);
        return;
    }
    double k = 1920.0 / W.view_w;
    W.act.video((int) (r[0] * k + 0.5), (int) (r[1] * k + 0.5), (int) (r[2] * k + 0.5), (int) (r[3] * k + 0.5));
}

static void free_stream(wm_stream *st) {
    drop_queue(st);
    close(st->fd);
    pthread_mutex_destroy(&st->wlock);
    free(st);
}

/* Feeder is done: get the readers out of their read(), wait for them, free everything */
static void free_session(wm_session *s) {
    pthread_join(s->thread, NULL);
    pthread_mutex_lock(&s->lock);
    for (int i = 0; i < 2; i++)
        if (s->st[i]) shutdown(s->st[i]->fd, SHUT_RDWR);
    int64_t t = now_ns();
    while (s->readers > 0 && now_ns() - t < 2000 * MS) timed_wait(&s->cond, &s->lock, 50);
    int left = s->readers;
    pthread_mutex_unlock(&s->lock);
    mt_log("[webmedia] session %s cleaned up (%d loads)%s", s->key, s->loads, left ? " - reader stuck, kept" : "");
    if (left) return; /* better a small leak than a use-after-free */
    for (int i = 0; i < 2; i++)
        if (s->st[i]) free_stream(s->st[i]);
    while (s->zombies) {
        wm_stream *z = s->zombies;
        s->zombies = z->zombie;
        free_stream(z);
    }
    free(s->ctx);
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond);
    free(s);
}

void webmedia_poll(void) {
    if (W.lfd < 0) return;
    wm_session *list[16];
    int n = 0;
    pthread_mutex_lock(&W.lock);
    for (wm_session **pp = &W.sessions; *pp;) {
        wm_session *s = *pp;
        if (s->finished) { /* feeder is done: remove from the list */
            *pp = s->next;
            pthread_mutex_unlock(&W.lock);
            free_session(s);
            pthread_mutex_lock(&W.lock);
            continue;
        }
        if (n < 16) list[n++] = s;
        pp = &s->next;
    }
    pthread_mutex_unlock(&W.lock);
    for (int i = 0; i < n; i++) {
        wm_session *s = list[i];
        pthread_mutex_lock(&s->lock);
        int want_api = s->req_api && !s->api, want_release = s->req_release, dirty = s->rect_dirty;
        int r[4] = {s->rect[0], s->rect[1], s->rect[2], s->rect[3]};
        s->rect_dirty = 0;
        pthread_mutex_unlock(&s->lock);
        if (want_release) release_api(s);
        if (want_api) create_api(s);
        if (dirty) apply_rect(r);
    }
    /* no playback any more: live picture back after a short quiet period (ad ->
     * programme item without a live interlude) */
    if (W.live_taken && !n) {
        int64_t now = now_ns();
        if (!W.idle_since) W.idle_since = now;
        else if (now - W.idle_since > 800 * MS) {
            W.live_taken = 0;
            W.idle_since = 0;
            mt_log("[webmedia] no more playback - live video back");
            W.act.video(0, 0, 0, 0);
            if (W.act.source) W.act.source(0, 0);
            W.act.live(1);
        }
    } else {
        W.idle_since = 0;
    }
}

int webmedia_active(void) {
    pthread_mutex_lock(&W.lock);
    int a = W.sessions != NULL;
    pthread_mutex_unlock(&W.lock);
    return a || W.live_taken;
}

void webmedia_set_ctype(const char *ctype) {
    snprintf(W.ctype, sizeof(W.ctype), "%s", ctype && *ctype ? ctype : "LIVE");
}

const char *webmedia_socket(void) { return W.lfd >= 0 ? W.name : ""; }

int webmedia_init(const webmedia_actions *a, void (*wakefn)(void), int view_w) {
    if (W.lfd >= 0) return 0;
    W.act = *a;
    W.wake = wakefn;
    W.view_w = view_w;
    /* TVF_WEBMEDIA_NAME: fixed name (test rig tools/webmedia-test) */
    const char *nm = getenv("TVF_WEBMEDIA_NAME");
    if (nm && nm[0] == '@' && nm[1]) snprintf(W.name, sizeof(W.name), "%s", nm);
    else snprintf(W.name, sizeof(W.name), "@tvf-media-%d", (int) getpid());
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    size_t nl = strlen(W.name + 1);
    if (nl > sizeof(sa.sun_path) - 2) nl = sizeof(sa.sun_path) - 2;
    memcpy(sa.sun_path + 1, W.name + 1, nl);
    socklen_t len = (socklen_t) (offsetof(struct sockaddr_un, sun_path) + 1 + nl);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *) &sa, len) != 0 || listen(fd, 8) != 0) {
        mt_log("[webmedia] socket %s: %s", W.name, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    W.lfd = fd;
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int r = pthread_create(&t, &at, accept_main, NULL);
    pthread_attr_destroy(&at);
    if (r != 0) {
        close(fd);
        W.lfd = -1;
        return -1;
    }
    mt_log("[webmedia] ready on %s (contentsType %s)", W.name, W.ctype);
    return 0;
}
