/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "player.h"

#include "hls.h"
#include "htsp.h"
#include "http.h"
#include "mpa_pcm.h"
#include "log.h"
#include "smp_api.h"
#include "ts.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <SDL.h>
#include <SDL_webOS.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define NS_PER_MS 1000000LL
#define PTS_MASK ((1LL << 33) - 1)

/* ------------------------------------------------------------------ */
/* Mini-JSON for the start parameters                                 */
/* ------------------------------------------------------------------ */

static const char *jfind(const char *json, const char *key) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    return p;
}

static int jint(const char *json, const char *key, int def) {
    const char *p = jfind(json, key);
    if (!p) return def;
    if (!strncmp(p, "true", 4)) return 1;
    if (!strncmp(p, "false", 5)) return 0;
    if (*p == '"') p++;
    return atoi(p);
}

static void jstr(const char *json, const char *key, char *out, size_t n, const char *def) {
    const char *p = jfind(json, key);
    if (!p || *p != '"') {
        snprintf(out, n, "%s", def);
        return;
    }
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < n) out[i++] = *p++;
    out[i] = 0;
}

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

typedef struct qitem {
    struct qitem *next;
    int video;
    int64_t pts90;
    size_t len;
    uint8_t data[];
} qitem;

typedef enum { ST_IDLE, ST_WAIT_KEY, ST_WAIT_AINFO, ST_FEEDING, ST_DONE } feed_state;

/* User pointer for Load(): when zapping, the teardown of the old pipeline runs
 * in parallel with the start of the new one - events of the old one must not
 * corrupt the new session. */
typedef struct {
    struct player *b;
    int gen;
    volatile int unloaded;
} cbctx;

struct player {
    /* Configuration */
    char user[64];
    char pass[64];
    player_htsp_status_fn htsp_status; /* status messages of the subscription, NULL = none */
    char url[2304];      /* current stream URL (http:// TS, htsp://host:port/<channel ID>, media:<type>:<url>) */
    char ctype[16];      /* transmission.contentsType, "none" = omit */
    int lowdelay;        /* -1 = omit */
    int audiosync;       /* -1 = omit */
    int pad;             /* esInfo.pauseAtDecodeTime */
    int margin_ms;       /* PTS base: the first keyframe sits at margin */
    int ptd0;            /* ptsToDecode 0 instead of margin */
    int vmax, vmin, amax, amin, prebuf;
    int audio_enabled;
    int force_apid;
    char alang[8];
    /* audio track choice: preference (per channel) and explicit choice for this URL */
    char pref_lang[4];
    int pref_type;
    char pref_codec[8];
    int sel_apid;
    /* file playback (recording) */
    int vod;
    long long vod_offset;
    double vod_pos0;
    int64_t vod_ref_pts90; /* first video PTS after the entry point = vod_pos0 */
    int vod_have_ref;
    volatile int feeder_eof; /* stream/file read to the end */
    volatile int feeder_err; /* ... but aborted before the end of the file (network), not finished */
    volatile int pipeline_lost; /* unloaded by the system (decoder handed to another app, e.g. HbbTV video) */
    volatile int eos_done;   /* ENDOFSTREAM of the pipeline */
    volatile long long file_size;
    int paused;

    char mpa_name[16];   /* Starfish codec name for MPEG-1/2 audio, "pcm" = decode ourselves */
    int cfg_maxw, cfg_maxh; /* preset from the start parameters, 0 = by codec */
    int maxw, maxh;
    int maxfps;          /* adaptiveStreaming.maxFrameRate (affects the output cadence!) */
    int fps;             /* esInfo.videoFpsValue, 0 = omit */
    int stats_ms;
    char dump[64];       /* raw data capture via TCP to "ip:port" */
    int dump_mb;
    int keymode;         /* 0: IDR/IRAP/I-with-SPS, 1: IDR/IRAP only, 2: also RAI only */
    char app_id[128];
    smp_acb_opts acb_opts; /* webOS 4: video plane via ACB (acbtype/acbwin/acbvdata) */

    pthread_mutex_t lock;
    pthread_cond_t cond;

    /* Window */
    char window_id[128];
    int have_window;
    int geo_pending, video_w, video_h;
    double video_fps; /* frameRate from the decoder's VIDEO_INFO */
    int interlaced;
    int dvb_onid, dvb_tsid, dvb_sid; /* DVB IDs from PAT/SDT, -1 = unknown (under lock) */
    /* media URL (HbbTV catch-up service): the pipeline loads by itself (URI/HLS/DASH) */
    int media;               /* running session is a media URL */
    int hls_mode;            /* running session is HLS (own fetching, TS feed) */
    double hls_start;        /* HLS VOD: start time (s) */
    volatile int media_playing, media_error;
    volatile int64_t media_dur_ms;
    int ev_range;
    int dst_x, dst_y, dst_w, dst_h; /* live picture in this window (HbbTV), dst_w = 0: full screen */
    int src_w, src_h;               /* picture size from outside (WebKit's playback), 0 = our own picture */

    /* Pipeline */
    smp_api *api;
    cbctx *api_ctx;      /* context of this API's last Load() */
    int api_refs;        /* outside calls currently using api (under lock, see api_get) */
    int gen;             /* session number */
    int loaded, load_completed, play_issued, unload_completed;

    /* Feeder */
    pthread_t thread;
    int thread_running;
    volatile int stop;
    int http_fd;
    volatile int http_status; /* response to the session's request, 0 = none */
    ts_demux ts;
    feed_state state;
    int vpid, apid;
    es_codec vcodec, acodec;
    ts_pmt cur_pmt;      /* under lock: the session's PMT for the audio track list */
    int have_pmt, cur_apid;
    unsigned long htsp_chid;                             /* channel of the HTSP session of cur_pmt */
    void (*pmt_request)(void *user, unsigned long chid); /* see player_set_pmt_request */
    void *pmt_request_user;
    es_audio_info ainfo;
    int have_ainfo;
    int64_t base90, ptd_ns;
    int pcm_mode;        /* MP2 -> PCM via minimp3 */
    mpa_pcm *mpa;
    qitem *qhead, *qtail;
    size_t qbytes;
    int64_t last_vpts_ns, last_apts_ns, last_fed_vpts_ns;

    /* timestamps of a session (ns since session start = app start or zap key) */
    int64_t s0, t_http, t_first_byte, t_pmt, t_key, t_load_call, t_load_ret, t_loadcompleted, t_playing,
        t_first_frame;
    int64_t key_pts_ns;

    /* Counters */
    uint64_t bytes_in, v_fed, a_fed, v_bytes, a_bytes, full_retries, v_drop_prekey, a_drop_early, feed_errors,
        no_pts, pes_cc_err;
    uint64_t frames, dropped_frames, ev_buffull, ev_buflow;
    uint64_t frames31;   /* event 0x31: frames shown per interval (webOS 25) */
    int bad_logged;
    uint64_t last_stats_frames31;
    int64_t last_frame_pts, last_frame_at;
    int64_t resumed_at; /* last resumed from pause (picture watchdog) */
    /* HTSP timeshift (tvh): requested/granted (s), state according to timeshiftStatus
     * (90 kHz, under lock); commands go to the subscription as htsp_sub_cmd */
    int htsp_ts_want, htsp_ts_granted;
    volatile int htsp_running; /* subscriptionStart received: commands possible */
    int ts_have, ts_full;
    int64_t ts_shift, ts_start, ts_end;
    int hsub_id;           /* subscriptionId of the running subscription (under lock), 0 = none */
    player_side side;      /* teletext/subtitles of the HTSP subscription */
    uint64_t side_bits;    /* stream indexes (< 64) of the side streams */
    int64_t vdrop_until; /* Test (debug VDROP): discard video until then */
    volatile int test_cut_fail; /* Test (debug CUT): make this many new requests fail */
    uint64_t last_stats_bytes, last_stats_frames;
    int64_t last_stats_at;

};

static void event_cb(int type, int64_t num, const char *str, void *user);

/* api for a call outside the feeder (main thread, pipeline events):
 * a reference under lock. Whoever takes the API out and destroys it (decode_restart in the
 * feeder, play_session/player_stop) waits with api_drain until nobody uses it
 * any more. loaded: only a loaded one; gen >= 0: only the one of this session */
static smp_api *api_get(player *b, int loaded, int gen) {
    pthread_mutex_lock(&b->lock);
    smp_api *a = b->api;
    if ((loaded && !b->loaded) || (gen >= 0 && gen != b->gen)) a = NULL;
    if (a) b->api_refs++;
    pthread_mutex_unlock(&b->lock);
    return a;
}

static void api_put(player *b, smp_api *a) {
    if (!a) return;
    pthread_mutex_lock(&b->lock);
    if (--b->api_refs == 0) pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->lock);
}

/* after taking it out (b->api = NULL under lock): until nobody uses the old API any more */
static void api_drain(player *b) {
    pthread_mutex_lock(&b->lock);
    while (b->api_refs > 0) pthread_cond_wait(&b->cond, &b->lock);
    pthread_mutex_unlock(&b->lock);
}

/* SDL_webOSSetExportedWindow only exists from webOS 5 on (webOS 4: video via
 * ACB) - looked up at runtime in player_create, see main.c */
static SDL_bool (*set_exported_window)(const char *, SDL_Rect *, SDL_Rect *);

/* ------------------------------------------------------------------ */

static const char *ev_name(int t) {
    switch (t) {
    case SMP_EV_FRAMEREADY: return "FRAMEREADY";
    case SMP_EV_STR_STREAMING_INFO: return "STREAMING_INFO";
    case SMP_EV_INT_BUFFER_RANGE: return "BUFFER_RANGE";
    case SMP_EV_INT_DURATION: return "DURATION";
    case SMP_EV_STR_VIDEO_INFO: return "VIDEO_INFO";
    case SMP_EV_STR_VIDEO_TRACK_INFO: return "VIDEO_TRACK_INFO";
    case SMP_EV_STR_AUDIO_INFO: return "AUDIO_INFO";
    case SMP_EV_STR_AUDIO_TRACK_INFO: return "AUDIO_TRACK_INFO";
    case SMP_EV_STR_BUFF_EVENT: return "BUFF_EVENT";
    case SMP_EV_STR_SOURCE_INFO: return "SOURCE_INFO";
    case SMP_EV_INT_NUM_PROGRAM: return "NUM_PROGRAM";
    case SMP_EV_INT_NUM_VIDEO_TRACK: return "NUM_VIDEO_TRACK";
    case SMP_EV_INT_NUM_AUDIO_TRACK: return "NUM_AUDIO_TRACK";
    case SMP_EV_STR_RESOURCE_INFO: return "RESOURCE_INFO";
    case SMP_EV_INT_ERROR: return "INT_ERROR";
    case SMP_EV_STR_ERROR: return "STR_ERROR";
    case SMP_EV_LOADCOMPLETED: return "LOADCOMPLETED";
    case SMP_EV_UNLOADCOMPLETED: return "UNLOADCOMPLETED";
    case SMP_EV_SEEKDONE: return "SEEKDONE";
    case SMP_EV_PLAYING: return "PLAYING";
    case SMP_EV_PAUSED: return "PAUSED";
    case SMP_EV_ENDOFSTREAM: return "ENDOFSTREAM";
    case SMP_EV_STR_CUSTOM: return "CUSTOM";
    case SMP_EV_INT_NEED_DATA: return "NEED_DATA";
    case SMP_EV_INT_ENOUGH_DATA: return "ENOUGH_DATA";
    case SMP_EV_INT_SVP_VDEC_READY: return "SVP_VDEC_READY";
    case SMP_EV_INT_BUFFERLOW: return "INT_BUFFERLOW";
    case SMP_EV_STR_BUFFERFULL: return "BUFFERFULL";
    case SMP_EV_STR_BUFFERLOW: return "BUFFERLOW";
    case SMP_EV_DROPPED_FRAME: return "DROPPED_FRAME";
    default: return "?";
    }
}

/* ms since session start, for the timeline in the log */
static double sms(const player *b, int64_t t) { return t ? (double) (t - b->s0) / 1e6 : -1.0; }

/* 90 kHz PTS relative to the base, as ns. Wraparound (33 bits) accounted for. */
static int64_t rebase_ns(const player *b, int64_t pts90) {
    int64_t d = (pts90 - b->base90) & PTS_MASK;
    if (d >= (1LL << 32)) d -= (1LL << 33);
    return d * 100000 / 9;
}

/* ------------------------------------------------------------------ */
/* Load()                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t cap, len;
} sbuf;

static void sb(sbuf *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void sb(sbuf *s, const char *fmt, ...) {
    if (s->len >= s->cap) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->len, s->cap - s->len, fmt, ap);
    va_end(ap);
    if (n > 0) s->len += (size_t) n;
}

static const char *video_name(es_codec c) {
    switch (c) {
    case ES_H264: return "H264";
    case ES_HEVC: return "H265";
    case ES_MPEG2V: return "MPEG2";
    default: return NULL;
    }
}

static const char *audio_name(const player *b, es_codec c) {
    switch (c) {
    case ES_AC3: return "AC3";
    case ES_EAC3: return "AC3 PLUS";
    case ES_AAC_ADTS:
    case ES_AAC_LATM: return "AAC";
    case ES_MPA: return b->pcm_mode ? "PCM" : b->mpa_name;
    case ES_DTS: return "DTS";
    default: return NULL;
    }
}

static int build_load_payload(player *b, char *out, size_t cap, int with_audio) {
    sbuf s = {out, cap, 0};
    const char *vname = video_name(b->vcodec);
    const char *aname = with_audio ? audio_name(b, b->acodec) : NULL;
    if (!vname) return -1;
    double freq = b->ainfo.sample_rate / 1000.0;

    sb(&s, "{\"args\":[{\"mediaTransportType\":\"BUFFERSTREAM\",\"option\":{");
    sb(&s, "\"appId\":\"%s\"", b->app_id);
    if (b->window_id[0]) sb(&s, ",\"windowId\":\"%s\"", b->window_id);
    sb(&s, ",\"needAudio\":%s", aname ? "true" : "false");
    sb(&s, ",\"queryPosition\":false,\"useDroppedFrameEvent\":true,\"seekMode\":\"keep-rate\"");
    if (strcmp(b->ctype, "none") != 0)
        sb(&s, ",\"transmission\":{\"contentsType\":\"%s\",\"trickType\":\"client-side\"}", b->ctype);
    if (b->lowdelay >= 0) sb(&s, ",\"lowDelayMode\":%s", b->lowdelay ? "true" : "false");

    sb(&s, ",\"externalStreamingInfo\":{\"contents\":{\"codec\":{\"video\":\"%s\"", vname);
    if (aname) sb(&s, ",\"audio\":\"%s\"", aname);
    sb(&s, "},\"esInfo\":{\"pauseAtDecodeTime\":%s,\"seperatedPTS\":true,\"ptsToDecode\":%lld",
       b->pad ? "true" : "false", (long long) b->ptd_ns);
    if (b->fps > 0) sb(&s, ",\"videoFpsValue\":%d,\"videoFpsScale\":1", b->fps);
    sb(&s, "}");
    sb(&s, ",\"format\":\"RAW\",\"provider\":\"%s\"", b->app_id);
    if (aname && b->acodec == ES_EAC3)
        sb(&s, ",\"ac3PlusInfo\":{\"channels\":%d,\"frequency\":%g}", b->ainfo.channels, freq);
    if (aname && b->pcm_mode)
        sb(&s, ",\"pcmInfo\":{\"format\":\"S16LE\",\"channelMode\":\"stereo\",\"layout\":\"interleaved\","
               "\"sampleRate\":%g,\"bitsPerSample\":16}", freq);
    if (aname && (b->acodec == ES_AAC_ADTS || b->acodec == ES_AAC_LATM))
        sb(&s, ",\"aacInfo\":{\"channels\":%d,\"profile\":%d,\"format\":\"%s\",\"frequency\":%g}", b->ainfo.channels,
           b->ainfo.aac_object_type ? b->ainfo.aac_object_type : 2, b->acodec == ES_AAC_LATM ? "latm" : "adts",
           freq);
    sb(&s, "}"); /* contents */
    sb(&s, ",\"streamQualityInfo\":true,\"streamQualityInfoNonFlushable\":true,"
           "\"streamQualityInfoCorruptedFrame\":true,\"restartStreaming\":false");
    if (b->audiosync >= 0) sb(&s, ",\"audioSync\":%s", b->audiosync ? "true" : "false");
    sb(&s, ",\"bufferingCtrInfo\":{\"preBufferByte\":%d,\"bufferMinLevel\":0,\"bufferMaxLevel\":0,"
           "\"qBufferLevelVideo\":0,\"qBufferLevelAudio\":0,"
           "\"srcBufferLevelVideo\":{\"minimum\":%d,\"maximum\":%d},"
           "\"srcBufferLevelAudio\":{\"minimum\":%d,\"maximum\":%d}}",
       b->prebuf, b->vmin, b->vmax, b->amin, b->amax);
    sb(&s, "}"); /* externalStreamingInfo */
    sb(&s, ",\"adaptiveStreaming\":{\"adaptiveResolution\":true,\"maxWidth\":%d,\"maxHeight\":%d,"
           "\"maxFrameRate\":%d}",
       b->maxw, b->maxh, b->maxfps);
    sb(&s, "}}]}");
    return s.len < cap ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* Feed                                                                */
/* ------------------------------------------------------------------ */

/* gen: from a pipeline event only for its session, -1 = feeder */
static void ensure_playing(player *b, int gen) {
    pthread_mutex_lock(&b->lock);
    int issue = !b->play_issued;
    pthread_mutex_unlock(&b->lock);
    if (!issue) return;
    smp_api *api = api_get(b, 0, gen);
    if (!api) return;
    int ok = smp_api_play(api);
    api_put(b, api);
    mt_log("[player] Play() -> %s", ok ? "accepted" : "rejected (will retry)");
    if (ok) {
        pthread_mutex_lock(&b->lock);
        b->play_issued = 1;
        pthread_mutex_unlock(&b->lock);
    }
}

static int feed(player *b, const uint8_t *data, size_t len, int64_t pts_ns, int es) {
    char reply[160];
    int64_t t_begin = 0;
    for (;;) {
        smp_feed_result r = smp_api_feed(b->api, data, (unsigned) len, pts_ns, es, reply, sizeof(reply));
        if (r == SMP_FEED_OK) {
            if (es == SMP_ES_VIDEO) {
                b->v_fed++;
                b->v_bytes += len;
                b->last_fed_vpts_ns = pts_ns;
                if (b->v_fed <= 3)
                    mt_log("[player] video #%llu fed: %zu bytes pts=%.3fs", (unsigned long long) b->v_fed, len,
                           pts_ns / 1e9);
            } else {
                b->a_fed++;
                b->a_bytes += len;
                if (b->a_fed <= 2)
                    mt_log("[player] audio #%llu fed: %zu bytes pts=%.3fs", (unsigned long long) b->a_fed, len,
                           pts_ns / 1e9);
            }
            ensure_playing(b, -1);
            return 0;
        }
        if (r == SMP_FEED_ERROR) {
            b->feed_errors++;
            /* pipeline unloaded by the system: feeding on is pointless */
            if (strstr(reply, "Unloading")) b->pipeline_lost = 1;
            if (b->feed_errors <= 10 || b->feed_errors % 500 == 0)
                mt_log("[player] Feed(%s, %zu B) error #%llu: %s", es == SMP_ES_VIDEO ? "V" : "A", len,
                       (unsigned long long) b->feed_errors, reply);
            return -1;
        }
        /* BufferFull/Pending: normal back pressure, retry the same buffer right away */
        b->full_retries++;
        if (b->stop) return -1;
        int64_t now = mt_now_ns();
        if (!t_begin || b->paused) t_begin = now; /* paused: back pressure is intended */
        if (now - t_begin > 5000 * NS_PER_MS) {
            mt_log("[player] feed stuck for 5 s (%s) - buffer dropped", reply);
            return -1;
        }
        usleep(5000);
    }
}

static void feed_video(player *b, const uint8_t *data, size_t len, int64_t pts90) {
    if (b->vdrop_until && mt_now_ns() < b->vdrop_until) return; /* test: data yes, picture no */
    int64_t ns;
    if (pts90 == TS_NO_PTS) {
        b->no_pts++;
        ns = b->last_vpts_ns + 20 * NS_PER_MS;
    } else {
        ns = rebase_ns(b, pts90);
    }
    if (b->last_vpts_ns && (ns - b->last_vpts_ns > 5000 * NS_PER_MS || b->last_vpts_ns - ns > 5000 * NS_PER_MS))
        mt_log("[player] video PTS jump: %.3fs -> %.3fs", b->last_vpts_ns / 1e9, ns / 1e9);
    b->last_vpts_ns = ns;
    if (ns < 0) { /* before the entry point (open GOP), not displayable */
        b->v_drop_prekey++;
        return;
    }
    feed(b, data, len, ns, SMP_ES_VIDEO);
}

static void feed_audio_ns(player *b, const uint8_t *data, size_t len, int64_t ns) {
    if (ns < b->ptd_ns) { /* audio before the first frame: discard, otherwise it runs ahead */
        b->a_drop_early++;
        return;
    }
    feed(b, data, len, ns, SMP_ES_AUDIO);
}

static void pcm_out(void *user, const int16_t *pcm, int samples, int64_t pts90, int sample_rate) {
    player *b = user;
    if (sample_rate != b->ainfo.sample_rate && b->a_fed == 0)
        mt_log("[player] PCM rate %d Hz differs from loaded rate (%d Hz)", sample_rate, b->ainfo.sample_rate);
    int64_t ns = rebase_ns(b, pts90);
    b->last_apts_ns = ns;
    feed_audio_ns(b, (const uint8_t *) pcm, (size_t) samples * 4, ns);
}

static void feed_audio(player *b, const uint8_t *data, size_t len, int64_t pts90) {
    if (b->pcm_mode) {
        if (!b->mpa) b->mpa = mpa_pcm_new();
        if (b->mpa) mpa_pcm_push(b->mpa, data, len, pts90 == TS_NO_PTS ? -1 : pts90, pcm_out, b);
        return;
    }
    int64_t ns;
    if (pts90 == TS_NO_PTS) {
        b->no_pts++;
        ns = b->last_apts_ns;
    } else {
        ns = rebase_ns(b, pts90);
    }
    b->last_apts_ns = ns;
    feed_audio_ns(b, data, len, ns);
}

/* ------------------------------------------------------------------ */
/* Queue before Load()                                                */
/* ------------------------------------------------------------------ */

static void q_push(player *b, int video, const uint8_t *data, size_t len, int64_t pts90) {
    qitem *it = malloc(sizeof(qitem) + len);
    if (!it) return;
    it->next = NULL;
    it->video = video;
    it->pts90 = pts90;
    it->len = len;
    memcpy(it->data, data, len);
    if (b->qtail) b->qtail->next = it;
    else b->qhead = it;
    b->qtail = it;
    b->qbytes += len;
    /* limit: drop the oldest entries (useless before the keyframe anyway) */
    while (b->qbytes > 32 * 1024 * 1024 && b->qhead) {
        qitem *h = b->qhead;
        b->qhead = h->next;
        if (!b->qhead) b->qtail = NULL;
        b->qbytes -= h->len;
        free(h);
    }
}

static void q_drop_video(player *b) {
    qitem **pp = &b->qhead;
    b->qtail = NULL;
    while (*pp) {
        qitem *it = *pp;
        if (it->video) {
            *pp = it->next;
            b->qbytes -= it->len;
            free(it);
        } else {
            b->qtail = it;
            pp = &it->next;
        }
    }
}

static void q_clear(player *b) {
    while (b->qhead) {
        qitem *h = b->qhead;
        b->qhead = h->next;
        free(h);
    }
    b->qtail = NULL;
    b->qbytes = 0;
}

static void do_load(player *b) {
    /* window and API must be there (they come from the main thread) */
    pthread_mutex_lock(&b->lock);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 5;
    while ((!b->have_window || !b->api) && !b->stop) {
        if (pthread_cond_timedwait(&b->cond, &b->lock, &dl) == ETIMEDOUT) break;
    }
    int ready = b->have_window && b->api;
    pthread_mutex_unlock(&b->lock);
    if (!ready) {
        mt_log("[player] no window/API after 5 s - aborting");
        b->state = ST_DONE;
        return;
    }

    int with_audio = b->apid >= 0 && b->have_ainfo && audio_name(b, b->acodec) != NULL;
    char payload[4096];
    if (build_load_payload(b, payload, sizeof(payload), with_audio) != 0) {
        mt_log("[player] Load payload does not fit / codec without Starfish name");
        b->state = ST_DONE;
        return;
    }
    smp_api_notify_foreground(b->api);
    mt_log("[player] Load: %s", payload);
    b->t_load_call = mt_now_ns();
    cbctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        b->state = ST_DONE;
        return;
    }
    ctx->b = b;
    ctx->gen = b->gen;
    pthread_mutex_lock(&b->lock);
    free(b->api_ctx); /* previous Load() of this API (does not happen, better safe than sorry) */
    b->api_ctx = ctx;
    pthread_mutex_unlock(&b->lock);
    int ok = smp_api_load(b->api, payload, event_cb, ctx);
    b->t_load_ret = mt_now_ns();
    mt_log("[player] Load() -> %s after %.1f ms (mediaId %s)", ok ? "ok" : "FAILED",
           (b->t_load_ret - b->t_load_call) / 1e6, smp_api_media_id(b->api) ? smp_api_media_id(b->api) : "-");
    if (!ok) {
        b->state = ST_DONE;
        return;
    }
    pthread_mutex_lock(&b->lock);
    b->loaded = 1;
    pthread_mutex_unlock(&b->lock);
    if (!with_audio && b->apid >= 0) {
        mt_log("[player] loaded without audio (codec %s)", es_codec_name(b->acodec));
        ts_select(&b->ts, b->vpid, -1);
        b->apid = -1;
        pthread_mutex_lock(&b->lock);
        b->cur_apid = -1;
        pthread_mutex_unlock(&b->lock);
    }
    b->state = ST_FEEDING;

    /* feed what was collected, in arrival order */
    size_t nq = 0;
    for (qitem *it = b->qhead; it && !b->stop; it = it->next) {
        if (it->video) feed_video(b, it->data, it->len, it->pts90);
        else if (b->apid >= 0) feed_audio(b, it->data, it->len, it->pts90);
        nq++;
    }
    q_clear(b);
    mt_log("[player] %zu queued PES fed after Load", nq);
}

/* ------------------------------------------------------------------ */
/* Demuxer callbacks (feeder thread)                                  */
/* ------------------------------------------------------------------ */

static const char *codec_key(es_codec c) {
    switch (c) {
    case ES_AC3: return "ac3";
    case ES_EAC3: return "eac3";
    case ES_MPA: return "mp2";
    case ES_AAC_ADTS: return "aac";
    case ES_AAC_LATM: return "latm";
    case ES_DTS: return "dts";
    default: return "";
    }
}

static int same_lang(const char *a, const char *b) {
    if (!a[0] || !b[0]) return 0;
    int ga = !strcmp(a, "ger") || !strcmp(a, "deu"), gb = !strcmp(b, "ger") || !strcmp(b, "deu");
    return (ga && gb) || !strncmp(a, b, 3);
}

/* Rank of an audio track: explicit choice > channel preference > German
 * without audio description; on a tie the better codec */
static int audio_rank(const player *b, const ts_es *e) {
    if (!es_is_audio(e->codec)) return -1;
    int r = 0;
    switch (e->codec) {
    case ES_AC3:
    case ES_EAC3: r = 40; break;
    case ES_AAC_ADTS: r = 30; break;
    case ES_AAC_LATM: r = 20; break;
    case ES_MPA: r = 10; break;
    default: r = 0; break;
    }
    if (b->sel_apid > 0 && e->pid == b->sel_apid) return 10000;
    if (b->pref_lang[0]) {
        if (same_lang(e->lang, b->pref_lang)) {
            r += 500;
            if (e->audio_type == b->pref_type) r += 200;
            if (!strcmp(codec_key(e->codec), b->pref_codec)) r += 100;
        }
    } else if (b->alang[0]) {
        if (same_lang(e->lang, b->alang)) r += 300;
    } else if (same_lang(e->lang, "ger")) {
        r += 300;
    }
    /* audio description, clear speech/hearing impaired only on request; a
     * supplementary track for mixing (on its own only the narrator's voice) last */
    if (e->audio_type == 2 || e->audio_type == 3 || !strcmp(e->lang, "mis") || !strcmp(e->lang, "qks")) r -= 200;
    if (e->receiver_mix) r = 0;
    return r;
}

static void on_pmt(void *user, const ts_pmt *pmt) {
    player *b = user;
    if (!b->t_pmt) b->t_pmt = mt_now_ns();
    mt_log("[ts] PMT program %d (PID %d, version %d), %d streams, after %.1f ms", pmt->program, pmt->pmt_pid,
           pmt->version, pmt->n, sms(b, b->t_pmt));
    int vi = -1, ai = -1, best = -1;
    for (int i = 0; i < pmt->n; i++) {
        const ts_es *e = &pmt->es[i];
        mt_log("[ts]   PID %4d type 0x%02x %-14s %s%s", e->pid, e->stream_type, es_codec_name(e->codec), e->lang,
               e->audio_type == 3 ? " (audio description)" : "");
        if (vi < 0 && es_is_video(e->codec)) vi = i;
        if (b->force_apid > 0 && e->pid == b->force_apid) {
            ai = i;
            best = 1000;
        }
        int r = audio_rank(b, e);
        if (r >= 0 && r > best) {
            best = r;
            ai = i;
        }
    }
    if (b->state != ST_WAIT_KEY && b->state != ST_IDLE) {
        /* PMT change during playback: only report it (prototype) */
        if (vi >= 0 && pmt->es[vi].pid != b->vpid) mt_log("[ts] PMT change: new video PID - not handled");
        return;
    }
    if (vi < 0) {
        mt_log("[ts] no video stream - aborting");
        b->state = ST_DONE;
        return;
    }
    b->vpid = pmt->es[vi].pid;
    b->vcodec = pmt->es[vi].codec;
    b->apid = (b->audio_enabled && ai >= 0 && best >= 0) ? pmt->es[ai].pid : -1;
    b->acodec = b->apid >= 0 ? pmt->es[ai].codec : ES_NONE;
    b->pcm_mode = (b->acodec == ES_MPA && !strcasecmp(b->mpa_name, "pcm"));
    b->maxw = b->cfg_maxw > 0 ? b->cfg_maxw : (b->vcodec == ES_HEVC ? 3840 : 1920);
    b->maxh = b->cfg_maxh > 0 ? b->cfg_maxh : (b->vcodec == ES_HEVC ? 2160 : 1080);
    mt_log("[ts] selected: video PID %d (%s), audio PID %d (%s %s)", b->vpid, es_codec_name(b->vcodec), b->apid,
           es_codec_name(b->acodec), b->apid >= 0 ? pmt->es[ai].lang : "");
    pthread_mutex_lock(&b->lock);
    b->cur_pmt = *pmt;
    b->have_pmt = 1;
    b->cur_apid = b->apid;
    pthread_mutex_unlock(&b->lock);
    ts_select(&b->ts, b->vpid, b->apid);
    b->state = ST_WAIT_KEY;
}

static void on_pes(void *user, int pid, const uint8_t *data, size_t len, int64_t pts, int64_t dts, int rai,
                   int cc_err) {
    (void) dts;
    player *b = user;
    if (b->stop || b->state == ST_DONE) return;
    if (cc_err) b->pes_cc_err++;
    int video = (pid == b->vpid);
    if (video && b->vod && !b->vod_have_ref && pts != TS_NO_PTS) {
        b->vod_ref_pts90 = pts;
        b->vod_have_ref = 1;
    }

    if (!video && !b->have_ainfo) {
        if (es_audio_info_parse(b->acodec, data, len, &b->ainfo) == 0) {
            b->have_ainfo = 1;
            mt_log("[ts] audio: %s %d Hz, %d channels%s", es_codec_name(b->acodec), b->ainfo.sample_rate,
                   b->ainfo.channels, b->acodec == ES_MPA ? (b->ainfo.layer == 2 ? ", Layer II" : ", Layer I/III") : "");
        }
    }

    if (b->state == ST_FEEDING) {
        if (video) feed_video(b, data, len, pts);
        else feed_audio(b, data, len, pts);
        return;
    }

    if (b->state == ST_WAIT_KEY) {
        if (!video) {
            q_push(b, 0, data, len, pts); /* audio can arrive before the video */
            return;
        }
        int k = es_keyframe(b->vcodec, data, len);
        int usable = (b->keymode == 0 && k >= 1) || (b->keymode == 1 && k == 2) || (b->keymode == 2 && (k || rai));
        if (!usable || pts == TS_NO_PTS) {
            b->v_drop_prekey++;
            return;
        }
        b->t_key = mt_now_ns();
        b->base90 = (pts - (int64_t) b->margin_ms * 90) & PTS_MASK;
        b->ptd_ns = b->ptd0 ? 0 : (int64_t) b->margin_ms * NS_PER_MS;
        b->key_pts_ns = rebase_ns(b, pts);
        mt_log("[ts] entry: %s (%s, rai=%d, %zu bytes) after %.1f ms, %llu video PES dropped before it, "
               "stream PTS %lld (%.3f s), stream pos ~%llu",
               k == 2 ? "IDR/IRAP" : (k == 1 ? "I-frame (open GOP)" : "RAI only"), es_codec_name(b->vcodec), rai, len,
               sms(b, b->t_key), (unsigned long long) b->v_drop_prekey, (long long) pts, pts / 90000.0,
               (unsigned long long) b->ts.offset);
        q_drop_video(b);
        q_push(b, 1, data, len, pts);
        b->state = ST_WAIT_AINFO;
    } else if (b->state == ST_WAIT_AINFO) {
        q_push(b, video, data, len, pts);
    }

    if (b->state == ST_WAIT_AINFO) {
        int64_t waited = mt_now_ns() - b->t_key;
        if (b->apid < 0 || b->have_ainfo || waited > 400 * NS_PER_MS) {
            if (b->apid >= 0 && !b->have_ainfo) mt_log("[player] no audio header after 400 ms - loading without audio");
            do_load(b);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Feeder thread                                                      */
/* ------------------------------------------------------------------ */

static int tcp_connect_hostport(const char *hp) {
    char host[64];
    const char *colon = strrchr(hp, ':');
    if (!colon || (size_t) (colon - hp) >= sizeof(host)) return -1;
    memcpy(host, hp, (size_t) (colon - hp));
    host[colon - hp] = 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) atoi(colon + 1));
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct timeval tv = {.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, (struct sockaddr *) &a, sizeof(a)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* http.c reports the socket of the running request even before connect():
 * this way stop_feeder() also aborts the wait for the response header immediately (tvh
 * does not answer at all for a channel without reception - otherwise the
 * next channel change would hang until the socket timeout). */
static int http_fd_hook(void *user, int fd) {
    player *b = user;
    pthread_mutex_lock(&b->lock);
    b->http_fd = fd;
    int stop = b->stop;
    pthread_mutex_unlock(&b->lock);
    return stop;
}

/* ------------------------------------------------------------------ */
/* HTSP: subscribe to the channel directly at tvh                     */
/* ------------------------------------------------------------------ */

/* Over HTSP, tvh delivers already split packets (one frame or one audio frame per
 * muxpkt, Annex B, ADTS, MP2/AC-3 raw) with 90 kHz timestamps - they go
 * to the same callbacks as the PES from the TS demuxer. The "PID" here is
 * tvh's stream index. With the dvbbuffer in tvh the subscription starts at the
 * last keyframe instead of a few seconds back. tvh normalizes the
 * timestamps (tsfix); teletext, subtitles and AIT come from the subscription itself
 * (player_set_side, sidedata.c) and therefore match the picture. */

static es_codec htsp_codec(const char *t) {
    static const struct {
        const char *name;
        es_codec c;
    } m[] = {{"H264", ES_H264},       {"HEVC", ES_HEVC},         {"MPEG2VIDEO", ES_MPEG2V},
             {"MPEG2AUDIO", ES_MPA},  {"AC3", ES_AC3},           {"EAC3", ES_EAC3},
             {"AAC", ES_AAC_ADTS},    {"TELETEXT", ES_TELETEXT}, {"DVBSUB", ES_DVBSUB},
             {"TEXTSUB", ES_TEXTSUB}};
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++)
        if (!strcmp(t, m[i].name)) return m[i].c;
    return ES_OTHER;
}

static const char *cjs(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : "";
}

static double cjn(const cJSON *o, const char *k, double def) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : def;
}

/* tvh: teletext page as decimal from the BCD digits (777) -> hex as in
 * teletext (0x777); 0 stays 0 */
static int dec2hex_page(int p) {
    if (p < 100 || p > 899) return 0;
    return (p / 100) << 8 | (p / 10 % 10) << 4 | p % 10;
}

/* subscriptionStart -> PMT for on_pmt; side streams to side */
static void htsp_start_msg(player *b, unsigned long chid, const unsigned char *body, size_t len) {
    cJSON *j = hmsg_parse(body, len);
    if (!j) return;
    ts_pmt pm;
    memset(&pm, 0, sizeof(pm));
    player_side_stream side[TS_MAX_ES];
    int nside = 0;
    uint64_t bits = 0;
    const cJSON *st;
    cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(j, "streams")) {
        if (pm.n >= TS_MAX_ES) break;
        ts_es *e = &pm.es[pm.n++];
        e->pid = (int) cjn(st, "index", -1);
        e->codec = htsp_codec(cjs(st, "type"));
        snprintf(e->lang, sizeof(e->lang), "%s", cjs(st, "language"));
        e->audio_type = (int) cjn(st, "audio_type", 0);
        /* tvh with the HTSP extensions (component, subtitle type, pages) */
        e->component_tag = (int) cjn(st, "component_tag", -1);
        e->receiver_mix = (int) cjn(st, "receiver_mix", 0);
        e->channels = (int) cjn(st, "channels", 0);
        if (e->codec == ES_DVBSUB) {
            e->sub_type = (int) cjn(st, "subtitling_type", 0);
            e->composition = (int) cjn(st, "composition_id", 0);
        }
        if (e->codec == ES_TEXTSUB) {
            e->sub_type = (int) cjn(st, "teletext_type", 0);
            e->ttx_page = dec2hex_page((int) cjn(st, "teletext_page", 0));
        }
        if (e->codec == ES_TELETEXT) e->ttx_initial = dec2hex_page((int) cjn(st, "teletext_initial_page", 0));
        if (es_is_video(e->codec))
            mt_log("[htsp]   stream %d %s %.0fx%.0f", e->pid, cjs(st, "type"), cjn(st, "width", 0), cjn(st, "height", 0));
        else if (e->codec == ES_DVBSUB)
            mt_log("[htsp]   stream %d DVBSUB %s page %.0f/%.0f type 0x%02x tag %d", e->pid, e->lang,
                   cjn(st, "composition_id", 0), cjn(st, "ancillary_id", 0), e->sub_type, e->component_tag);
        else if (e->codec == ES_TEXTSUB)
            mt_log("[htsp]   stream %d TEXTSUB %s page %03X type %d tag %d", e->pid, e->lang, e->ttx_page, e->sub_type,
                   e->component_tag);
        else
            mt_log("[htsp]   stream %d %s %s%s%s%s tag %d", e->pid, cjs(st, "type"), e->lang,
                   e->audio_type == 3 ? " (AD)" : e->audio_type == 2 ? " (hearing impaired)" : "",
                   e->receiver_mix ? " (receiver mix)" : "",
                   e->codec == ES_TELETEXT && e->ttx_initial ? " (initial page)" : "", e->component_tag);
        int ait = !strcmp(cjs(st, "type"), "HBBTV");
        /* TEXTSUB only as information (page, type), the others with packets (index < 64) */
        if ((e->codec == ES_TEXTSUB || ((e->codec == ES_TELETEXT || e->codec == ES_DVBSUB || ait) && e->pid < 64)) &&
            e->pid >= 0 && nside < TS_MAX_ES) {
            player_side_stream *s = &side[nside++];
            memset(s, 0, sizeof(*s));
            s->index = e->pid;
            s->teletext = e->codec == ES_TELETEXT;
            s->textsub = e->codec == ES_TEXTSUB;
            s->ait = ait;
            snprintf(s->lang, sizeof(s->lang), "%s", e->lang);
            s->composition = (int) cjn(st, "composition_id", 0);
            s->ancillary = (int) cjn(st, "ancillary_id", 0);
            s->sub_type = e->sub_type;
            s->ttx_page = e->ttx_page;
            s->ttx_initial = e->ttx_initial;
            if (!s->textsub) bits |= 1ULL << e->pid;
        }
    }
    b->side_bits = b->side.data ? bits : 0;
    if (b->side.start) b->side.start(b->side.user, chid, side, nside);
    /* a tvh without the HTSP extensions: nothing of what they send in any stream */
    int ext = 0;
    for (int i = 0; i < pm.n; i++) {
        const ts_es *e = &pm.es[i];
        ext |= e->component_tag >= 0 || e->receiver_mix || e->sub_type || e->ttx_page || e->ttx_initial;
    }
    b->htsp_chid = chid;
    const cJSON *si = cJSON_GetObjectItemCaseSensitive(j, "sourceinfo");
    mt_log("[htsp] subscriptionStart after %.1f ms: %s / %s, %d streams", sms(b, mt_now_ns()), cjs(si, "mux"),
           cjs(si, "service"), pm.n);
    if (b->htsp_status) b->htsp_status(chid, "subscriptionStart", j);
    cJSON_Delete(j);
    on_pmt(b, &pm);
    if (!ext && b->pmt_request) b->pmt_request(b->pmt_request_user, chid);
}

static void htsp_status_msg(player *b, unsigned long chid, const char *method, const unsigned char *body,
                            size_t len) {
    cJSON *j = hmsg_parse(body, len);
    if (!j) return;
    if (b->htsp_status) b->htsp_status(chid, method, j);
    if (!strcmp(method, "subscriptionStatus") || !strcmp(method, "subscriptionStop"))
        mt_log("[htsp] %s after %.1f ms: %s%s%s", method, sms(b, mt_now_ns()), cjs(j, "status"),
               cjs(j, "subscriptionError")[0] ? " / " : "", cjs(j, "subscriptionError"));
    else if (!strcmp(method, "subscriptionGrace"))
        mt_log("[htsp] subscriptionGrace %.0f s", cjn(j, "graceTimeout", 0));
    cJSON_Delete(j);
}

static void unload_api(player *b, smp_api *api, cbctx *ctx, int loaded);

/* Timeshift skip (HTSP): new pipeline, the connection stays. Tracks,
 * codecs and audio info remain valid; from the next keyframe on like a start */
static void decode_restart(player *b) {
    int64_t t = mt_now_ns();
    pthread_mutex_lock(&b->lock);
    smp_api *old = b->api;
    cbctx *old_ctx = b->api_ctx;
    int old_loaded = b->loaded;
    b->api = NULL;
    b->api_ctx = NULL;
    b->loaded = b->load_completed = b->play_issued = b->unload_completed = 0;
    __atomic_add_fetch(&b->gen, 1, __ATOMIC_RELEASE); /* discard events of the old pipeline */
    b->frames = 0;
    b->last_frame_pts = b->last_frame_at = 0;
    pthread_mutex_unlock(&b->lock);
    api_drain(b); /* the main thread (pause, window) may be using it right now */
    unload_api(b, old, old_ctx, old_loaded);
    smp_api_destroy(old);
    free(old_ctx);
    smp_api *api = smp_api_create();
    if (api) smp_api_notify_foreground(api);
    q_clear(b);
    mpa_pcm_free(b->mpa);
    b->mpa = NULL;
    b->state = ST_WAIT_KEY;
    b->base90 = 0;
    b->ptd_ns = 0;
    b->last_vpts_ns = b->last_apts_ns = b->last_fed_vpts_ns = 0;
    b->v_fed = b->a_fed = b->v_drop_prekey = b->a_drop_early = 0;
    b->s0 = t; /* time the restart like a zap */
    b->t_key = b->t_load_call = b->t_load_ret = b->t_loadcompleted = b->t_playing = b->t_first_frame = 0;
    b->t_first_byte = b->t_http = t;
    b->paused = 0;
    b->resumed_at = t;
    pthread_mutex_lock(&b->lock);
    b->api = api;
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->lock);
    mt_log("[player] decoder restarted after %.1f ms (timeshift jump)", (mt_now_ns() - t) / 1e6);
}

#define HTSP_STALL_S 10 /* running HTSP subscription without a message for this long: ended (reconnect) */
static void htsp_feeder(player *b) {
    /* htsp://host:port/<channelId>[?profile=...]; the subscription goes over the
     * shared HTSP connection (htsp.c), host:port are only in the URL */
    char host[64] = "", profile[64] = "";
    int port = 9982;
    unsigned long chid = 0;
    const char *h = b->url + 7, *sl = strchr(h, '/');
    if (sl) {
        const char *colon = memchr(h, ':', (size_t) (sl - h));
        size_t hl = (size_t) ((colon ? colon : sl) - h);
        if (hl < sizeof(host)) {
            memcpy(host, h, hl);
            host[hl] = 0;
        }
        if (colon) port = atoi(colon + 1);
        chid = strtoul(sl + 1, NULL, 10);
        const char *q = strstr(sl, "profile=");
        if (q) snprintf(profile, sizeof(profile), "%.*s", (int) strcspn(q + 8, "&"), q + 8);
    }
    if (!host[0] || !chid) {
        mt_log("[htsp] unusable URL: %s", b->url);
        b->state = ST_DONE;
        return;
    }
    (void) port;
    int sid = 0;
    htsp_sub *hs = b->stop ? NULL : htsp_sub_open((uint32_t) chid, b->htsp_ts_want, profile, &sid);
    if (!hs) {
        if (!b->stop) mt_log("[htsp] no connection to tvh %s", host);
        b->state = ST_DONE;
        return;
    }
    pthread_mutex_lock(&b->lock);
    b->hsub_id = sid;
    int stopping = b->stop; /* stop_feeder came first: it could not wake the subscription yet */
    pthread_mutex_unlock(&b->lock);
    if (stopping) htsp_sub_cancel(sid);
    mt_log("[htsp] subscribe channel %lu (subscription %d)%s%s%s after %.1f ms", chid, sid,
           profile[0] ? " profile " : "", profile, b->htsp_ts_want > 0 ? ", timeshift" : "", sms(b, mt_now_ns()));

    ts_init(&b->ts, on_pmt, on_pes, b); /* only for ts_select/counters */
    b->state = ST_IDLE;
    int started = 0;
    uint64_t pkts = 0;
    while (!b->stop) {
        size_t len = 0;
        int gone = 0;
        /* no time limit until the start (cancel: htsp_sub_cancel) */
        unsigned char *body = htsp_sub_read(hs, &len, started ? HTSP_STALL_S * 1000 : -1, &gone);
        if (!body && !gone && !b->stop && started && b->paused) continue; /* paused (timeshift): tvh sends nothing */
        if (!body) {
            if (!b->stop && !gone) mt_log("[htsp] no message from tvh for %d s - subscription ended", HTSP_STALL_S);
            else if (!b->stop) mt_log("[htsp] connection ended after %.1f ms", sms(b, mt_now_ns()));
            break;
        }
        /* method comes first; parse muxpkt without cJSON */
        size_t pos = 0;
        hmsg_field f;
        char method[32] = "";
        int stream = -1, ftype = 0;
        int64_t pts = TS_NO_PTS, dts = TS_NO_PTS;
        const unsigned char *pl = NULL;
        size_t pll = 0;
        while (hmsg_next(body, len, &pos, &f) == 1) {
            if (hmsg_is(&f, "method")) snprintf(method, sizeof(method), "%.*s", (int) f.dlen, (const char *) f.data);
            else if (hmsg_is(&f, "stream")) stream = (int) hmsg_s64_of(&f);
            else if (hmsg_is(&f, "pts")) pts = hmsg_s64_of(&f);
            else if (hmsg_is(&f, "dts")) dts = hmsg_s64_of(&f);
            else if (hmsg_is(&f, "frametype")) ftype = (int) hmsg_s64_of(&f);
            else if (hmsg_is(&f, "payload")) {
                pl = f.data;
                pll = f.dlen;
            }
        }
        if (!strcmp(method, "muxpkt")) {
            if (!b->t_first_byte) {
                b->t_first_byte = mt_now_ns();
                mt_log("[htsp] first packet after %.1f ms", sms(b, b->t_first_byte));
            }
            pkts++;
            b->bytes_in += pll;
            if (pl && stream >= 0 && (stream == b->vpid || (b->apid >= 0 && stream == b->apid)))
                on_pes(b, stream, pl, pll, pts >= 0 ? (pts & PTS_MASK) : TS_NO_PTS,
                       dts >= 0 ? (dts & PTS_MASK) : TS_NO_PTS, ftype == 'I', 0);
            else if (pl && stream >= 0 && stream < 64 && (b->side_bits >> stream & 1))
                b->side.data(b->side.user, stream, pl, pll, pts >= 0 ? (pts & PTS_MASK) : -1);
        } else if (!strcmp(method, "subscriptionStart")) {
            b->t_http = mt_now_ns();
            b->http_status = 200;
            started = 1;
            b->htsp_running = 1; /* running subscription: silent this long = stalled (HTSP_STALL_S) */
            htsp_start_msg(b, chid, body, len);
        } else if (!strcmp(method, "subscriptionStop")) {
            htsp_status_msg(b, chid, method, body, len);
            if (!started) b->http_status = 503;
            free(body);
            break;
        } else if (!strcmp(method, "timeshiftStatus")) {
            cJSON *j = hmsg_parse(body, len);
            if (!b->ts_have)
                mt_log("[htsp] timeshiftStatus: shift %.1f s, buffer %.1f..%.1f s%s", cjn(j, "shift", 0) / 90000.0,
                       cjn(j, "start", -1) / 90000.0, cjn(j, "end", -1) / 90000.0, cjn(j, "full", 0) ? ", full" : "");
            pthread_mutex_lock(&b->lock);
            b->ts_have = 1;
            b->ts_full = (int) cjn(j, "full", 0);
            b->ts_shift = (int64_t) cjn(j, "shift", 0);
            b->ts_start = (int64_t) cjn(j, "start", -1);
            b->ts_end = (int64_t) cjn(j, "end", -1);
            pthread_mutex_unlock(&b->lock);
            cJSON_Delete(j);
        } else if (!strcmp(method, "subscriptionSkip")) {
            /* tvh skips within the timeshift: from here on the packets of the new
             * position arrive - re-initialize the decoder, the connection (the buffer) stays */
            cJSON *j = hmsg_parse(body, len);
            mt_log("[htsp] subscriptionSkip%s%s, time %.1f s", cjn(j, "error", 0) ? " ERROR" : "",
                   cjn(j, "absolute", 0) ? " absolute" : "", cjn(j, "time", 0) / 90000.0);
            cJSON_Delete(j);
            decode_restart(b);
            if (b->side.skip) b->side.skip(b->side.user);
        } else if (method[0]) {
            htsp_status_msg(b, chid, method, body, len);
        } else if (!b->htsp_ts_granted) {
            /* reply to subscribe: granted timeshift */
            cJSON *j = hmsg_parse(body, len);
            b->htsp_ts_granted = (int) cjn(j, "timeshiftPeriod", 0);
            if (b->htsp_ts_want > 0)
                mt_log("[htsp] timeshift %s (%d s)", b->htsp_ts_granted ? "granted" : "not available", b->htsp_ts_granted);
            cJSON_Delete(j);
        }
        free(body);
        if (b->pipeline_lost) {
            mt_log("[player] pipeline unloaded by system (decoder taken by another app) - session ended");
            break;
        }
    }
    b->htsp_running = 0;
    mt_log("[htsp] subscription end: %llu packets, %.1f MB", (unsigned long long) pkts, b->bytes_in / 1048576.0);
    b->feeder_eof = 1;
    pthread_mutex_lock(&b->lock);
    b->hsub_id = 0;
    pthread_mutex_unlock(&b->lock);
    htsp_sub_close(hs); /* unsubscribe; the connection stays */
    q_clear(b);
    ts_free(&b->ts);
}

/* JSON string (without quotes) */
static void json_esc(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 7 < n; in++) {
        unsigned char c = (unsigned char) *in;
        if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char) c;
        } else if (c < 0x20) {
            o += (size_t) snprintf(out + o, n - o, "\\u%04x", c);
        } else {
            out[o++] = (char) c;
        }
    }
    out[o] = 0;
}

/* Media URL ("media:<type>:<url>", type URI/HLS/MPEG-DASH): the pipeline fetches
 * and decodes by itself - for the catch-up services of the HbbTV pages. Here we only load,
 * play and wait until the end/stop. */
static void media_feeder(player *b) {
    const char *p = b->url + 6, *colon = strchr(p, ':');
    if (!colon) {
        b->state = ST_DONE;
        return;
    }
    char type[16], uri[2048];
    snprintf(type, sizeof(type), "%.*s", (int) (colon - p), p);
    json_esc(colon + 1, uri, sizeof(uri));
    b->media = 1;
    b->t_http = mt_now_ns();
    pthread_mutex_lock(&b->lock);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 5;
    while ((!b->have_window || !b->api) && !b->stop) {
        if (pthread_cond_timedwait(&b->cond, &b->lock, &dl) == ETIMEDOUT) break;
    }
    int ready = b->have_window && b->api;
    pthread_mutex_unlock(&b->lock);
    if (!ready || b->stop) {
        if (!b->stop) mt_log("[player] media: no window/API after 5 s");
        b->state = ST_DONE;
        return;
    }
    char payload[4096];
    snprintf(payload, sizeof(payload),
             "{\"args\":[{\"mediaTransportType\":\"%s\",\"uri\":\"%s\",\"option\":{\"appId\":\"%s\"%s%s%s,"
             "\"needAudio\":true,\"queryPosition\":true,\"transmission\":{\"contentsType\":\"VOD\"},"
             "\"adaptiveStreaming\":{\"adaptiveResolution\":true,\"maxWidth\":%d,\"maxHeight\":%d,\"maxFrameRate\":%d}}}]}",
             type, uri, b->app_id, b->window_id[0] ? ",\"windowId\":\"" : "", b->window_id,
             b->window_id[0] ? "\"" : "", 3840, 2160, b->maxfps);
    smp_api_notify_foreground(b->api);
    mt_log("[player] media Load: %s", payload);
    b->t_load_call = mt_now_ns();
    cbctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        b->state = ST_DONE;
        return;
    }
    ctx->b = b;
    ctx->gen = b->gen;
    pthread_mutex_lock(&b->lock);
    free(b->api_ctx);
    b->api_ctx = ctx;
    pthread_mutex_unlock(&b->lock);
    int ok = smp_api_load(b->api, payload, event_cb, ctx);
    b->t_load_ret = mt_now_ns();
    mt_log("[player] media Load() -> %s after %.1f ms", ok ? "ok" : "FAILED", (b->t_load_ret - b->t_load_call) / 1e6);
    if (!ok) {
        b->media_error = -1;
        b->state = ST_DONE;
        return;
    }
    pthread_mutex_lock(&b->lock);
    b->loaded = 1;
    pthread_mutex_unlock(&b->lock);
    b->state = ST_FEEDING;
    /* errors do not end the session: some codes are only warnings (202
     * right after the Load) - the page decides, TVF passes them on */
    while (!b->stop && !b->eos_done) usleep(50 * 1000);
    if (b->eos_done) b->feeder_eof = 1;
    mt_log("[player] media session %s", b->stop ? "stopped" : b->eos_done ? "ended" : "ended with error");
}

static void *feeder_main(void *arg) {
    player *b = arg;
    if (!strncmp(b->url, "htsp://", 7)) {
        htsp_feeder(b);
        return NULL;
    }
    if (!strncmp(b->url, "media:", 6)) {
        media_feeder(b);
        return NULL;
    }
    char url[sizeof(b->url)];
    snprintf(url, sizeof(url), "%s", b->url);

    http_conn *c = calloc(1, sizeof(http_conn));
    if (!c) return NULL;
    c->fd = -1;
    http_hook hook = {http_fd_hook, b};
    int rc;
    hls *hl = NULL;
    if (!strncmp(url, "hls:", 4)) {
        /* HLS (catch-up service): segments as a continuous TS stream */
        char err[160] = "";
        hl = hls_open(url + 4, b->hls_start, &hook, &b->stop, err, sizeof(err));
        rc = hl ? 0 : -1;
        c->status = hl ? 200 : 0;
        snprintf(c->content_type, sizeof(c->content_type), "%s", hl ? "HLS" : "-");
        if (!hl && !b->stop) mt_log("[player] HLS: %s", err);
        if (hl) {
            b->media_dur_ms = (int64_t) (hls_duration(hl) * 1000);
            if (!hls_is_live(hl)) {
                b->vod = 1; /* position from the PTS, pushEOS at the end */
                b->vod_pos0 = hls_start_pos(hl);
            }
        }
    } else {
        rc = http_open_ex(url, b->user, b->pass, b->vod && b->vod_offset > 0 ? b->vod_offset : -1, &hook, c);
    }
    b->http_status = c->status;
    if (rc == 0 && b->vod && !hl) {
        b->file_size = c->total_size;
        mt_log("[player] file from byte %lld (%.1f s), size %lld", b->vod_offset, b->vod_pos0, c->total_size);
    }
    b->t_http = mt_now_ns();
    if (rc != 0) {
        if (b->stop) mt_log("[player] HTTP aborted after %.1f ms", sms(b, b->t_http));
        else mt_log("[player] HTTP failed (status %d) after %.1f ms", c->status, sms(b, b->t_http));
        free(c);
        b->state = ST_DONE;
        return NULL;
    }
    mt_log("[player] HTTP %d after %.1f ms, Content-Type %s", c->status, sms(b, b->t_http), c->content_type);
    if (strstr(c->content_type, "mp4"))
        mt_log("[player] WARNING: server delivers MP4 instead of TS - check profile/user (profile=pass allowed?)");
    /* the socket is already registered in b->http_fd via the hook */

    ts_init(&b->ts, on_pmt, on_pes, b);
    pthread_mutex_lock(&b->lock);
    b->dvb_onid = b->dvb_tsid = b->dvb_sid = -1;
    pthread_mutex_unlock(&b->lock);
    b->state = ST_IDLE;
    int dump_fd = b->dump[0] ? tcp_connect_hostport(b->dump) : -1;
    size_t dump_left = (size_t) b->dump_mb * 1024 * 1024;
    if (b->dump[0]) mt_log("[player] dump to %s: %s", b->dump, dump_fd >= 0 ? "connected" : "FAILED");
    static const size_t BUFSZ = 188 * 512;
    uint8_t *buf = malloc(BUFSZ);
    /* file (recording): next byte and end of file, for restarting */
    int file = b->vod && !hl;
    long long pos = b->vod_offset > 0 ? b->vod_offset : 0, end = file ? c->total_size : -1;
    int retries = 0;
    while (buf && !b->stop) {
        ssize_t n = hl ? hls_read(hl, buf, BUFSZ) : http_read(c, buf, BUFSZ);
        int err = errno;
        if (n <= 0 && file && !b->stop && (n < 0 || (end > 0 && pos < end))) {
            /* aborted before the end of the file (Wi-Fi gone, 8 s without data, tvh closed
             * after a long pause): not the end of the file - request again from the same
             * byte, the TS stream continues seamlessly */
            if (retries < 5) {
                retries++;
                mt_log("[player] file cut at byte %lld of %lld (%s) - reconnecting (%d/5)", pos, end,
                       n == 0 ? "EOF" : strerror(err), retries);
                pthread_mutex_lock(&b->lock);
                b->http_fd = -1; /* unregister before http_close (http_hook) */
                pthread_mutex_unlock(&b->lock);
                http_close(c);
                memset(c, 0, sizeof(*c));
                c->fd = -1;
                for (int w = 0; w < retries * 10 && !b->stop; w++) usleep(100 * 1000);
                int rc2 = -1;
                if (b->test_cut_fail > 0) {
                    b->test_cut_fail--;
                    mt_log("[player] test: reconnect refused");
                } else if (!b->stop) {
                    rc2 = http_open_ex(url, b->user, b->pass, pos, &hook, c);
                }
                if (rc2 == 0 && c->status == 206) {
                    if (c->total_size > 0) end = b->file_size = c->total_size; /* a running recording grows */
                    mt_log("[player] file resumed at byte %lld", pos);
                    continue;
                }
                if (!b->stop) mt_log("[player] reconnect failed (status %d)", c->status);
                if (rc2 == 0) { /* response without Range (200): starting from the beginning would be wrong */
                    pthread_mutex_lock(&b->lock);
                    b->http_fd = -1;
                    pthread_mutex_unlock(&b->lock);
                    http_close(c);
                }
                memset(c, 0, sizeof(*c));
                c->fd = -1;
                continue; /* the next read fails: next attempt */
            }
            b->feeder_err = 1;
        }
        if (n <= 0) {
            if (!b->stop) mt_log("[player] stream ended (%s)", n == 0 ? "EOF" : strerror(err));
            if (!b->stop && b->vod && n == 0 && !b->feeder_err && b->state == ST_FEEDING) {
                int ok = smp_api_push_eos(b->api);
                mt_log("[player] end of file: pushEOS -> %s", ok ? "ok" : "FAILED");
            }
            break;
        }
        if (!b->t_first_byte) {
            b->t_first_byte = mt_now_ns();
            mt_log("[player] first data after %.1f ms", sms(b, b->t_first_byte));
        }
        b->bytes_in += (uint64_t) n;
        pos += n;
        retries = 0;
        uint64_t bad_before = b->ts.pes[0].bad_start + b->ts.pes[1].bad_start + b->ts.pes[0].scrambled +
                              b->ts.pes[1].scrambled;
        if (dump_fd >= 0) {
            size_t m = (size_t) n < dump_left ? (size_t) n : dump_left;
            if (send(dump_fd, buf, m, MSG_NOSIGNAL) != (ssize_t) m) {
                close(dump_fd);
                dump_fd = -1;
            } else if ((dump_left -= m) == 0) {
                close(dump_fd);
                dump_fd = -1;
                mt_log("[player] dump finished (%d MB)", b->dump_mb);
            }
        }
        ts_feed(&b->ts, buf, (size_t) n);
        if (b->ts.chosen >= 0) {
            int sid = b->ts.prog[b->ts.chosen].program;
            if (sid != b->dvb_sid || b->ts.tsid != b->dvb_tsid || b->ts.onid != b->dvb_onid) {
                pthread_mutex_lock(&b->lock);
                b->dvb_sid = sid;
                b->dvb_tsid = b->ts.tsid;
                b->dvb_onid = b->ts.onid;
                pthread_mutex_unlock(&b->lock);
            }
        }
        if (b->pipeline_lost) {
            /* e.g. an HbbTV page plays its own video: session over,
             * the app restarts when the decoder is free again */
            mt_log("[player] pipeline unloaded by system (decoder taken by another app) - session ended");
            break;
        }
        uint64_t bad_after = b->ts.pes[0].bad_start + b->ts.pes[1].bad_start + b->ts.pes[0].scrambled +
                             b->ts.pes[1].scrambled;
        if (bad_after != bad_before && b->bad_logged < 40) {
            b->bad_logged++;
            mt_log("[ts] INTEGRITY: V scrambled %llu / bad PES header %llu (pos %llu), A scrambled %llu / bad %llu "
                   "(pos %llu)",
                   (unsigned long long) b->ts.pes[0].scrambled, (unsigned long long) b->ts.pes[0].bad_start,
                   (unsigned long long) b->ts.pes[0].last_bad_off, (unsigned long long) b->ts.pes[1].scrambled,
                   (unsigned long long) b->ts.pes[1].bad_start, (unsigned long long) b->ts.pes[1].last_bad_off);
        }
    }
    free(buf);
    b->feeder_eof = 1;
    if (dump_fd >= 0) close(dump_fd);
    pthread_mutex_lock(&b->lock);
    b->http_fd = -1;
    pthread_mutex_unlock(&b->lock);
    if (hl) hls_close(hl);
    http_close(c);
    free(c);
    q_clear(b);
    ts_free(&b->ts);
    return NULL;
}

static void reset_session(player *b) {
    b->state = ST_IDLE;
    b->http_status = 0;
    mpa_pcm_free(b->mpa);
    b->mpa = NULL;
    b->pcm_mode = 0;
    b->vpid = b->apid = -1;
    b->side_bits = 0;
    b->vcodec = b->acodec = ES_NONE;
    b->have_ainfo = 0;
    memset(&b->ainfo, 0, sizeof(b->ainfo));
    b->base90 = 0;
    b->ptd_ns = 0;
    b->last_vpts_ns = b->last_apts_ns = b->last_fed_vpts_ns = 0;
    b->t_http = b->t_first_byte = b->t_pmt = b->t_key = b->t_load_call = b->t_load_ret = 0;
    b->t_loadcompleted = b->t_playing = b->t_first_frame = 0;
    b->bytes_in = b->v_fed = b->a_fed = b->v_bytes = b->a_bytes = b->full_retries = 0;
    b->v_drop_prekey = b->a_drop_early = b->feed_errors = b->no_pts = b->pes_cc_err = 0;
    b->frames = b->dropped_frames = b->ev_buffull = b->ev_buflow = 0;
    b->frames31 = b->last_stats_frames31 = 0;
    b->bad_logged = 0;
    b->last_frame_pts = b->last_frame_at = 0;
    b->resumed_at = 0;
    b->last_stats_bytes = b->last_stats_frames = 0;
    b->loaded = b->load_completed = b->play_issued = b->unload_completed = 0;
    b->vod_have_ref = 0;
    b->vod_ref_pts90 = 0;
    b->feeder_eof = 0;
    b->feeder_err = 0;
    b->pipeline_lost = 0;
    b->eos_done = 0;
    b->media = 0;
    b->media_playing = 0;
    b->media_error = 0;
    b->media_dur_ms = 0;
    b->ev_range = 0;
    b->paused = 0;
    b->htsp_running = 0;
    b->htsp_ts_granted = 0;

    pthread_mutex_lock(&b->lock);
    b->have_pmt = 0;
    b->cur_apid = -1;
    b->ts_have = b->ts_full = 0;
    b->ts_shift = 0;
    b->ts_start = b->ts_end = -1;
    pthread_mutex_unlock(&b->lock);
}

static void start_feeder(player *b, int64_t s0) {
    b->stop = 0;
    b->s0 = s0;
    b->last_stats_at = b->s0;
    if (pthread_create(&b->thread, NULL, feeder_main, b) == 0) b->thread_running = 1;
    else mt_log("[player] thread start failed");
}

static void stop_feeder(player *b) {
    b->stop = 1;
    pthread_mutex_lock(&b->lock);
    if (b->http_fd >= 0) shutdown(b->http_fd, SHUT_RDWR); /* wakes connect()/recv() */
    if (b->hsub_id) htsp_sub_cancel(b->hsub_id);           /* wakes htsp_sub_read */
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->lock);
    if (b->thread_running) {
        pthread_join(b->thread, NULL);
        b->thread_running = 0;
    }
}

/* Unloads an (old or current) pipeline and waits for UNLOADCOMPLETED. */
static void unload_api(player *b, smp_api *api, cbctx *ctx, int loaded) {
    if (!api || !loaded) return;
    int64_t t = mt_now_ns();
    int ok = smp_api_unload(api);
    pthread_mutex_lock(&b->lock);
    struct timespec dl;
    clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += 1;
    while (ctx && !ctx->unloaded) {
        if (pthread_cond_timedwait(&b->cond, &b->lock, &dl) == ETIMEDOUT) break;
    }
    int done = ctx ? ctx->unloaded : 0;
    pthread_mutex_unlock(&b->lock);
    mt_log("[player] Unload() -> %s, UNLOADCOMPLETED %s after %.1f ms", ok ? "ok" : "FAILED",
           done ? "yes" : "NO (timeout)", (mt_now_ns() - t) / 1e6);
}

/* ------------------------------------------------------------------ */
/* Pipeline events (pipeline thread)                                  */
/* ------------------------------------------------------------------ */

static void first_frame_summary(player *b) {
    mt_log("[player] *** FIRST PICTURE after %.1f ms *** (HTTP %.1f, data %.1f, PMT %.1f, keyframe %.1f, "
           "Load %.1f->%.1f, LOADCOMPLETED %.1f, PLAYING %.1f; keyframe PTS %.3fs, picture PTS %.3fs)",
           sms(b, b->t_first_frame), sms(b, b->t_http), sms(b, b->t_first_byte), sms(b, b->t_pmt), sms(b, b->t_key),
           sms(b, b->t_load_call), sms(b, b->t_load_ret), sms(b, b->t_loadcompleted), sms(b, b->t_playing),
           b->key_pts_ns / 1e9, b->last_frame_pts / 1e9);
}

/* events whose str is a valid text (for INT_* it is not!) */
static int ev_has_str(int t) {
    return t == 0x01 || t == 0x04 || t == 0x05 || t == 0x07 || t == 0x08 || t == 0x09 || t == 0x0a || t == 0x0b ||
           t == 0x11 || t == 0x13 || (t >= 0x15 && t <= 0x1d) || t == 0x2d || t == 0x2e || t >= 0x2710;
}

static void event_cb(int type, int64_t num, const char *str, void *user) {
    cbctx *ctx = user;
    player *b = ctx->b;
    int64_t now = mt_now_ns();
    if (!ev_has_str(type)) str = NULL;
    if (type == SMP_EV_UNLOADCOMPLETED) {
        pthread_mutex_lock(&b->lock);
        ctx->unloaded = 1;
        pthread_cond_broadcast(&b->cond);
        pthread_mutex_unlock(&b->lock);
        mt_log("[player] UNLOADCOMPLETED (session %d)", ctx->gen);
        return;
    }
    if (ctx->gen != __atomic_load_n(&b->gen, __ATOMIC_ACQUIRE)) return; /* old pipeline */
    if (type == 0x31) {
        b->frames31 += num > 0 ? (uint64_t) num : 0;
        return;
    }
    switch (type) {
    case SMP_EV_FRAMEREADY:
        pthread_mutex_lock(&b->lock); /* 64 bits: avoid torn reads in the main thread */
        b->frames++;
        b->last_frame_pts = num;
        b->last_frame_at = now;
        pthread_mutex_unlock(&b->lock);
        if (b->frames == 1) {
            b->t_first_frame = now;
            first_frame_summary(b);
        }
        return;
    case SMP_EV_LOADCOMPLETED:
        b->t_loadcompleted = now;
        mt_log("[player] LOADCOMPLETED after %.1f ms (%.1f ms after Load)", sms(b, now), (now - b->t_load_call) / 1e6);
        pthread_mutex_lock(&b->lock);
        b->load_completed = 1;
        b->play_issued = 0; /* an earlier Play() can only have been queued */
        pthread_mutex_unlock(&b->lock);
        ensure_playing(b, ctx->gen);
        return;
    case SMP_EV_PLAYING:
        if (!b->t_playing) b->t_playing = now;
        b->media_playing = 1;
        /* media URL: no FRAMEREADY events - PLAYING counts as the first frame */
        if (b->media && !b->t_first_frame) b->t_first_frame = now;
        mt_log("[player] PLAYING after %.1f ms", sms(b, now));
        return;
    case SMP_EV_ENDOFSTREAM:
        b->eos_done = 1;
        b->media_playing = 0;
        mt_log("[player] ENDOFSTREAM");
        return;
    case SMP_EV_PAUSED:
        b->media_playing = 0;
        mt_log("[player] PAUSED");
        return;
    case SMP_EV_INT_DURATION:
        b->media_dur_ms = num;
        mt_log("[player] DURATION %lld", (long long) num);
        return;
    case SMP_EV_DROPPED_FRAME:
        b->dropped_frames += num > 0 ? (uint64_t) num : 1;
        if (b->dropped_frames <= 5) mt_log("[player] DROPPED_FRAME %lld", (long long) num);
        return;
    case SMP_EV_STR_BUFFERFULL:
        b->ev_buffull++;
        if (b->ev_buffull <= 3) mt_log("[player] event BUFFERFULL %s", str ? str : "");
        return;
    case SMP_EV_STR_BUFFERLOW:
    case SMP_EV_INT_BUFFERLOW:
        b->ev_buflow++;
        if (b->ev_buflow <= 3) mt_log("[player] event %s %lld %s", ev_name(type), (long long) num, str ? str : "");
        return;
    case SMP_EV_STR_VIDEO_INFO: {
        mt_log("[player] VIDEO_INFO %s", str ? str : "");
        const char *w = str ? strstr(str, "\"width\"") : NULL;
        const char *h = str ? strstr(str, "\"height\"") : NULL;
        if (w && h) {
            int vw = atoi(strchr(w, ':') + 1), vh = atoi(strchr(h, ':') + 1);
            if (vw > 0 && vh > 0) {
                const char *fr = strstr(str, "\"frameRate\"");
                pthread_mutex_lock(&b->lock);
                b->video_w = vw;
                b->video_h = vh;
                b->video_fps = fr ? atof(strchr(fr, ':') + 1) : 0;
                b->interlaced = strstr(str, "\"VIDEO_INTERLACED\"") != NULL;
                b->geo_pending = 1;
                smp_api *api = ctx->gen == b->gen ? b->api : NULL;
                if (api) b->api_refs++;
                pthread_mutex_unlock(&b->lock);
                if (api) smp_api_video_info(api, str, vw, vh); /* webOS 4: ACB sets up the video plane with it */
                api_put(b, api);
            }
        }
        return;
    }
    case SMP_EV_INT_ERROR:
    case SMP_EV_STR_ERROR:
        if (b->media) b->media_error = num ? (int) num : -1;
        if (b->media && type == SMP_EV_STR_ERROR) mt_log("[player] media error text %s", str ? str : "-");
        mt_log("[player] PIPELINE ERROR %s %lld %s (last shown: pts %.3f s = stream PTS %lld / %.3f s, "
               "fed up to %.3f s, stream pos %llu)",
               ev_name(type), (long long) num, str ? str : "", b->last_frame_pts / 1e9,
               (long long) ((b->base90 + b->last_frame_pts * 9 / 100000) & PTS_MASK),
               ((b->base90 + b->last_frame_pts * 9 / 100000) & PTS_MASK) / 90000.0, b->last_fed_vpts_ns / 1e9,
               (unsigned long long) b->bytes_in);
        if (num == 601) mt_log("[player] 601 = decoder busy (another media app open or previous run without Unload)");
        return;
    case SMP_EV_INT_BUFFER_RANGE:
        if (++b->ev_range <= 3) mt_log("[player] event BUFFER_RANGE num=%lld %.200s", (long long) num, str ? str : "");
        return;
    default:
        mt_log("[player] event %s (0x%02x) num=%lld %.300s", ev_name(type), type, (long long) num, str ? str : "");
        return;
    }
}

/* ------------------------------------------------------------------ */
/* Public interface                                                   */
/* ------------------------------------------------------------------ */

player *player_create(const char *p) {
    player *b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    if (!p) p = "{}";
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->cond, NULL);
    b->http_fd = -1;
    jstr(p, "user", b->user, sizeof(b->user), "");
    jstr(p, "pass", b->pass, sizeof(b->pass), "");
    jstr(p, "ctype", b->ctype, sizeof(b->ctype), "LIVE");
    b->lowdelay = jint(p, "lowdelay", -1);
    b->audiosync = jint(p, "audiosync", -1);
    b->pad = jint(p, "pad", 1);
    b->margin_ms = jint(p, "margin", 1000);
    b->ptd0 = jint(p, "ptd0", 0);
    b->vmax = jint(p, "vmax", 8 * 1024 * 1024);
    b->vmin = jint(p, "vmin", 0);
    b->amax = jint(p, "amax", 2 * 1024 * 1024);
    b->amin = jint(p, "amin", 0);
    b->prebuf = jint(p, "prebuf", 0);
    b->audio_enabled = jint(p, "audio", 1);
    b->force_apid = jint(p, "apid", 0);
    jstr(p, "alang", b->alang, sizeof(b->alang), "");
    jstr(p, "mpa", b->mpa_name, sizeof(b->mpa_name), "pcm");
    b->cfg_maxw = jint(p, "maxw", 0);
    b->cfg_maxh = jint(p, "maxh", 0);
    b->stats_ms = jint(p, "stats", 0);
    jstr(p, "dump", b->dump, sizeof(b->dump), "");
    b->dump_mb = jint(p, "dumpmb", 8);
    b->maxfps = jint(p, "maxfps", 60);
    b->fps = jint(p, "fps", 0);
    b->keymode = jint(p, "keymode", 0);
    b->acb_opts.player_type = jint(p, "acbtype", 10); /* PLAYER_TYPE_MSE */
    b->acb_opts.window = jint(p, "acbwin", 0);
    b->acb_opts.video_data = jint(p, "acbvdata", 1);
    const char *appid = getenv("APPID");
    snprintf(b->app_id, sizeof(b->app_id), "%s", appid && *appid ? appid : "com.lsd.tvf");
    void *self = dlopen(NULL, RTLD_NOW);
    if (self)
        set_exported_window =
            (SDL_bool (*)(const char *, SDL_Rect *, SDL_Rect *)) dlsym(self, "SDL_webOSSetExportedWindow");
    reset_session(b);
    mt_log("[player] settings: ctype=%s lowdelay=%d audiosync=%d pad=%d margin=%dms ptd0=%d vbuf=%d..%d "
           "abuf=%d..%d prebuf=%d audio=%d mpa=%s keymode=%d maxfps=%d fps=%d appId=%s",
           b->ctype, b->lowdelay, b->audiosync, b->pad, b->margin_ms, b->ptd0, b->vmin, b->vmax, b->amin, b->amax,
           b->prebuf, b->audio_enabled, b->mpa_name, b->keymode, b->maxfps, b->fps, b->app_id);
    return b;
}

int player_use_acb(player *b) { return smp_api_acb_enable(b->app_id, &b->acb_opts); }

void player_set_video_rect(player *b, int x, int y, int w, int h) {
    pthread_mutex_lock(&b->lock);
    b->dst_x = x;
    b->dst_y = y;
    b->dst_w = w > 0 && h > 0 ? w : 0;
    b->dst_h = h;
    b->geo_pending = 1; /* exported window: in player_tick */
    smp_api *api = b->api;
    if (api) b->api_refs++;
    pthread_mutex_unlock(&b->lock);
    smp_api_set_rect(api, x, y, w > 0 && h > 0 ? w : 0, h); /* ACB (webOS 4); remembered even without api */
    api_put(b, api);
}

/* Source of the exported window = picture of another decoder user (WebKit's
 * playback via webmedia.c): its size instead of that of the last live picture,
 * just as the player takes it for its own picture from VIDEO_INFO. 0 = back to our own */
void player_set_video_source(player *b, int w, int h) {
    pthread_mutex_lock(&b->lock);
    if (w <= 0 || h <= 0) w = h = 0;
    if (w != b->src_w || h != b->src_h) {
        b->src_w = w;
        b->src_h = h;
        b->geo_pending = 1;
    }
    pthread_mutex_unlock(&b->lock);
}

void player_set_window(player *b, const char *window_id) {
    pthread_mutex_lock(&b->lock);
    snprintf(b->window_id, sizeof(b->window_id), "%s", window_id ? window_id : "");
    b->have_window = 1;
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->lock);
}

/*
 * New session: the running pipeline is taken out and torn down in parallel with
 * the start of the new one - HTTP, PMT and keyframe search are already running
 * while the old pipeline is unloaded here. Load() waits in the feeder
 * until the new API is ready.
 */
static void play_session(player *b, const char *url) {
    int64_t t = mt_now_ns();
    char logurl[1024];
    http_url_redact(url, logurl, sizeof(logurl));
    mt_log("[player] ===== Start: %s =====", logurl);
    stop_feeder(b);
    int64_t t_stopped = mt_now_ns();

    pthread_mutex_lock(&b->lock);
    smp_api *old = b->api;
    cbctx *old_ctx = b->api_ctx;
    int old_loaded = b->loaded;
    b->api = NULL;
    b->api_ctx = NULL;
    __atomic_add_fetch(&b->gen, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&b->lock);
    reset_session(b);
    snprintf(b->url, sizeof(b->url), "%s", url);
    b->hls_mode = !strncmp(url, "hls:", 4);
    start_feeder(b, t); /* session time from the call: start/zap time */

    api_drain(b); /* a pipeline event may be using it right now */
    unload_api(b, old, old_ctx, old_loaded);
    int64_t t_unloaded = mt_now_ns();
    smp_api_destroy(old);
    free(old_ctx);
    smp_api *api = smp_api_create();
    int fg = api ? smp_api_notify_foreground(api) : 0;
    pthread_mutex_lock(&b->lock);
    b->api = api;
    pthread_cond_broadcast(&b->cond);
    pthread_mutex_unlock(&b->lock);
    mt_log("[player] parallel teardown: feeder %.1f ms, Unload %.1f ms, new API %.1f ms (notifyForeground %s)",
           (t_stopped - t) / 1e6, (t_unloaded - t_stopped) / 1e6, (mt_now_ns() - t_unloaded) / 1e6,
           fg ? "ok" : "-");
}

void player_play(player *b, const char *url) {
    b->sel_apid = 0; /* an explicit audio track choice only applies to this stream */
    b->vod = 0;
    b->vod_offset = 0;
    b->vod_pos0 = 0;
    play_session(b, url);
}

void player_play_media(player *b, const char *type, const char *url) {
    char u[sizeof(b->url)];
    snprintf(u, sizeof(u), "media:%s:%s", type, url);
    b->sel_apid = 0;
    b->vod = 0;
    b->vod_offset = 0;
    b->vod_pos0 = 0;
    b->paused = 0;
    play_session(b, u);
}

void player_play_hls(player *b, const char *url, double start) {
    char u[sizeof(b->url)];
    snprintf(u, sizeof(u), "hls:%s", url);
    b->sel_apid = 0;
    b->vod = 0;
    b->vod_offset = 0;
    b->vod_pos0 = start > 0 ? start : 0;
    b->paused = 0;
    b->hls_start = start > 0 ? start : 0; /* before the start: the feeder reads it */
    play_session(b, u);
}

void player_media_get(player *b, player_media *m) {
    memset(m, 0, sizeof(*m));
    if (b->hls_mode) {
        /* HLS through our own feed: state as with live/recording */
        m->active = b->thread_running && b->state != ST_DONE && !b->eos_done;
        m->playing = b->t_first_frame && !b->paused && !b->eos_done;
        m->ended = b->eos_done || (b->state == ST_DONE && b->t_first_frame);
        m->error = b->state == ST_DONE && !b->t_first_frame && !b->stop ? -1 : 0;
        m->dur_ms = b->media_dur_ms;
        double p = player_position(b);
        if (p < 0 && b->t_first_frame) {
            pthread_mutex_lock(&b->lock);
            int64_t fpts = b->last_frame_pts;
            pthread_mutex_unlock(&b->lock);
            p = fpts / 1e9; /* live: running time since start */
        }
        m->pos_ms = p >= 0 ? (int64_t) (p * 1000) : -1;
        return;
    }
    smp_api *api = api_get(b, 1, -1);
    m->active = b->media && b->thread_running;
    m->playing = b->media_playing;
    m->ended = b->eos_done;
    m->error = b->media_error;
    m->dur_ms = b->media_dur_ms;
    m->pos_ms = -1;
    if (m->active && api) {
        int64_t t = smp_api_current_playtime(api); /* ns */
        if (t >= 0) m->pos_ms = t / 1000000;
    }
    api_put(b, api);
}

void player_media_seek(player *b, double sec) {
    smp_api *api = b->media ? api_get(b, 1, -1) : NULL;
    if (!api) return;
    char ms[32];
    snprintf(ms, sizeof(ms), "%lld", (long long) (sec * 1000));
    int ok = smp_api_seek(api, ms);
    api_put(b, api);
    mt_log("[player] Seek(%s ms) -> %s", ms, ok ? "ok" : "FAILED");
}

void player_play_file(player *b, const char *url, long long offset, double pos0) {
    /* same file (seek): keep the audio track choice */
    if (!b->vod || strcmp(url, b->url) != 0) b->sel_apid = 0;
    b->vod = 1;
    b->vod_offset = offset;
    b->vod_pos0 = pos0;
    b->file_size = -1;
    play_session(b, url);
}

/* same channel, different entry point (timeshift): keep the audio track choice */
void player_play_keep_audio(player *b, const char *url) {
    b->vod = 0;
    b->vod_offset = 0;
    b->vod_pos0 = 0;
    play_session(b, url);
}

void player_test_drop_video(player *b, int seconds) {
    b->vdrop_until = seconds > 0 ? mt_now_ns() + (int64_t) seconds * 1000 * NS_PER_MS : 0;
    mt_log("[player] test: dropping video for %d s", seconds);
}

void player_set_paused(player *b, int paused) {
    smp_api *api = api_get(b, 1, -1);
    b->paused = paused;
    if (!paused) b->resumed_at = mt_now_ns();
    if (!api) return;
    int ok = paused ? smp_api_pause(api) : smp_api_play(api);
    api_put(b, api);
    mt_log("[player] %s -> %s", paused ? "Pause()" : "Play()", ok ? "ok" : "FAILED");
}

int player_paused(player *b) { return b->paused; }

void player_set_htsp_timeshift(player *b, int seconds) { b->htsp_ts_want = seconds > 0 ? seconds : 0; }

void player_set_pmt_request(player *b, void (*cb)(void *user, unsigned long chid), void *user) {
    b->pmt_request = cb;
    b->pmt_request_user = user;
}

/* the k-th stream of the PMT that fits a stream of the HTSP list: same kind, for audio also the
 * codec and language, a better fit (same audio type) first */
static int pmt_match(const ts_es *e, const ts_pmt *src, const int *used) {
    int first = -1;
    for (int j = 0; j < src->n; j++) {
        const ts_es *c = &src->es[j];
        if (used[j]) continue;
        int fit;
        if (es_is_video(e->codec)) fit = es_is_video(c->codec);
        else if (es_is_audio(e->codec))
            fit = c->codec == e->codec && (!e->lang[0] || !c->lang[0] || same_lang(e->lang, c->lang));
        else if (e->codec == ES_DVBSUB)
            fit = c->codec == ES_DVBSUB && (!e->composition || !c->composition || e->composition == c->composition);
        else fit = c->codec == e->codec;
        if (!fit) continue;
        if (es_is_audio(e->codec) && e->audio_type == c->audio_type) return j;
        if (first < 0) first = j;
    }
    return first;
}

int player_merge_pmt(player *b, unsigned long channel, const ts_pmt *src) {
    int changed = 0, used[TS_MAX_ES] = {0}, ttxsub = 0, ttx_tag = -1;
    pthread_mutex_lock(&b->lock);
    if (!b->have_pmt || b->htsp_chid != channel) {
        pthread_mutex_unlock(&b->lock);
        return 0;
    }
    ts_pmt *dst = &b->cur_pmt;
    ts_pmt before = *dst;
    for (int i = 0; i < dst->n; i++) {
        ts_es *e = &dst->es[i];
        if (e->codec == ES_TEXTSUB || e->codec == ES_OTHER || e->codec == ES_NONE) continue;
        int j = pmt_match(e, src, used);
        if (j < 0) continue;
        const ts_es *c = &src->es[j];
        used[j] = 1;
        if (e->component_tag < 0 && c->component_tag >= 0) e->component_tag = c->component_tag, changed = 1;
        if (!e->receiver_mix && c->receiver_mix) e->receiver_mix = 1, changed = 1;
        if (es_is_audio(e->codec) && !e->audio_type && c->audio_type) e->audio_type = c->audio_type, changed = 1;
        if (e->codec == ES_DVBSUB && !e->sub_type && c->sub_type) e->sub_type = c->sub_type, changed = 1;
        if (e->codec == ES_TELETEXT) {
            if (!e->ttx_initial && c->ttx_initial) e->ttx_initial = c->ttx_initial, changed = 1;
            ttx_tag = c->component_tag;
        }
    }
    /* the teletext subtitle pages: one stream each, in the order of the PMT (same language) */
    for (int i = 0; i < dst->n; i++) {
        ts_es *e = &dst->es[i];
        if (e->codec != ES_TEXTSUB) continue;
        if (e->component_tag < 0 && ttx_tag >= 0) e->component_tag = ttx_tag, changed = 1;
        if (e->ttx_page) continue;
        for (int k = ttxsub; k < src->n_ttxsub; k++) {
            const ts_ttxsub *t = &src->ttxsub[k];
            if (e->lang[0] && t->lang[0] && !same_lang(e->lang, t->lang)) continue;
            e->ttx_page = t->page;
            e->sub_type = t->type;
            ttxsub = k + 1;
            changed = 1;
            break;
        }
    }
    pthread_mutex_unlock(&b->lock);
    if (changed) {
        mt_log("[player] stream details from the PMT of channel %lu added:", channel);
        for (int i = 0; i < before.n; i++) {
            const ts_es *o = &before.es[i], *n = &dst->es[i];
            if (o->component_tag != n->component_tag || o->receiver_mix != n->receiver_mix ||
                o->audio_type != n->audio_type || o->sub_type != n->sub_type || o->ttx_page != n->ttx_page ||
                o->ttx_initial != n->ttx_initial)
                mt_log("[player]   stream %d %s %s: tag %d, audio type %d%s, subtitle type 0x%02x, page %03X, initial %03X",
                       n->pid, es_codec_name(n->codec), n->lang, n->component_tag, n->audio_type,
                       n->receiver_mix ? " (receiver mix)" : "", n->sub_type, n->ttx_page, n->ttx_initial);
        }
    }
    return changed;
}

void player_set_side(player *b, const player_side *s) {
    if (s) b->side = *s;
    else memset(&b->side, 0, sizeof(b->side));
}

int player_htsp_timeshift(player *b, player_tshift *out) {
    memset(out, 0, sizeof(*out));
    if (!b->htsp_running || !b->htsp_ts_granted) return 0;
    pthread_mutex_lock(&b->lock);
    out->have_status = b->ts_have;
    out->full = b->ts_full;
    out->shift90 = b->ts_shift;
    out->start90 = b->ts_start;
    out->end90 = b->ts_end;
    pthread_mutex_unlock(&b->lock);
    out->period = b->htsp_ts_granted;
    return 1;
}

/* command to the running subscription; field/value optional */
static int htsp_cmd(player *b, const char *method, const char *field, int64_t value, const char *field2,
                    int64_t value2) {
    if (!b->htsp_running) return -1;
    pthread_mutex_lock(&b->lock);
    int sid = b->hsub_id;
    pthread_mutex_unlock(&b->lock);
    if (!sid) return -1;
    hmsg m;
    hmsg_init(&m);
    hmsg_str(&m, "method", method);
    if (field) hmsg_s64(&m, field, value);
    if (field2) hmsg_s64(&m, field2, value2);
    int r = htsp_sub_cmd(sid, &m);
    hmsg_free(&m);
    mt_log("[htsp] %s%s%s -> %s", method, field ? " " : "", field ? field : "", r == 0 ? "sent" : "FAILED");
    return r;
}

int player_htsp_speed(player *b, int speed) { return htsp_cmd(b, "subscriptionSpeed", "speed", speed, NULL, 0); }

int player_htsp_skip(player *b, long long delta90) {
    return htsp_cmd(b, "subscriptionSkip", "time", delta90, "absolute", 0);
}

int player_htsp_live(player *b) { return htsp_cmd(b, "subscriptionLive", NULL, 0, NULL, 0); }

double player_position(player *b) {
    if (!b->vod) return -1;
    pthread_mutex_lock(&b->lock);
    uint64_t frames = b->frames;
    int64_t fpts = b->last_frame_pts;
    pthread_mutex_unlock(&b->lock);
    if (!frames || !b->vod_have_ref) return b->vod_pos0;
    int64_t abs90 = (b->base90 + fpts * 9 / 100000) & PTS_MASK;
    int64_t d = (abs90 - b->vod_ref_pts90) & PTS_MASK;
    if (d >= (1LL << 32)) d -= (1LL << 33);
    return b->vod_pos0 + d / 90000.0;
}

long long player_stream_pts(player *b) {
    pthread_mutex_lock(&b->lock);
    uint64_t frames = b->frames;
    int64_t fpts = b->last_frame_pts, at = b->last_frame_at;
    pthread_mutex_unlock(&b->lock);
    if (!frames || b->state == ST_DONE) return -1;
    int64_t ns = fpts;
    if (!b->paused) {
        int64_t el = mt_now_ns() - at;
        if (el > 0 && el < 2000 * NS_PER_MS) ns += el; /* runs in real time */
    }
    return (long long) ((b->base90 + ns * 9 / 100000) & PTS_MASK);
}

int player_ended(player *b) {
    if (!b->vod) return 0;
    if (b->eos_done) return 1;
    pthread_mutex_lock(&b->lock);
    int quiet = b->frames && mt_now_ns() - b->last_frame_at > 3000 * NS_PER_MS;
    pthread_mutex_unlock(&b->lock);
    if (!b->feeder_eof || b->paused) return 0;
    if (b->feeder_err) return quiet || !b->frames ? 2 : 0; /* even without a single frame: do not wait forever */
    return quiet;
}

void player_test_cut(player *b, int fail) {
    pthread_mutex_lock(&b->lock);
    b->test_cut_fail = fail > 0 ? fail : 0;
    int fd = b->http_fd;
    if (fd >= 0) shutdown(fd, SHUT_RDWR); /* recv returns 0: like a disconnect by the server */
    pthread_mutex_unlock(&b->lock);
    mt_log("[player] test: connection %s, next %d reconnects fail", fd >= 0 ? "cut" : "- none", fail);
}

void player_select_audio_file(player *b, int pid, long long offset, double pos0) {
    char url[sizeof(b->url)];
    snprintf(url, sizeof(url), "%s", b->url);
    if (!url[0]) return;
    mt_log("[player] audio track PID %d: reopening file at %.1f s", pid, pos0);
    b->sel_apid = pid;
    b->vod = 1;
    b->vod_offset = offset;
    b->vod_pos0 = pos0;
    b->file_size = -1;
    play_session(b, url);
}

long long player_file_size(player *b) { return b->file_size; }

void player_set_htsp_status(player *b, player_htsp_status_fn fn) { b->htsp_status = fn; }

void player_set_audio_pref(player *b, const char *lang, int type, const char *codec) {
    snprintf(b->pref_lang, sizeof(b->pref_lang), "%s", lang ? lang : "");
    b->pref_type = type;
    snprintf(b->pref_codec, sizeof(b->pref_codec), "%s", codec ? codec : "");
}

int player_audio_tracks(player *b, player_track *out, int max, int *current) {
    int n = 0;
    if (current) *current = -1;
    pthread_mutex_lock(&b->lock);
    for (int i = 0; b->have_pmt && i < b->cur_pmt.n && n < max; i++) {
        const ts_es *e = &b->cur_pmt.es[i];
        if (!es_is_audio(e->codec)) continue;
        player_track *t = &out[n];
        t->pid = e->pid;
        snprintf(t->lang, sizeof(t->lang), "%s", e->lang);
        t->type = e->audio_type;
        t->receiver_mix = e->receiver_mix;
        snprintf(t->codec, sizeof(t->codec), "%s", codec_key(e->codec));
        if (current && e->pid == b->cur_apid) *current = n;
        n++;
    }
    pthread_mutex_unlock(&b->lock);
    return n;
}

void player_select_audio(player *b, int pid) {
    char url[sizeof(b->url)];
    snprintf(url, sizeof(url), "%s", b->url);
    if (!url[0]) return;
    mt_log("[player] audio track PID %d: new session", pid);
    b->sel_apid = pid;
    play_session(b, url);
}

const char *player_window_id(player *b) { return b->window_id; }

/* OIPF encoding of a stream */
static const char *oipf_encoding(es_codec c) {
    switch (c) {
    case ES_H264: return "H264";
    case ES_HEVC: return "HEVC";
    case ES_MPEG2V: return "MPEG2";
    case ES_MPA: return "MPEG1_L2";
    case ES_AAC_ADTS:
    case ES_AAC_LATM: return "HEAAC";
    case ES_AC3: return "AC3";
    case ES_EAC3: return "E-AC3";
    case ES_DTS: return "DTS";
    case ES_DVBSUB: return "DVB-SUBT";
    case ES_TEXTSUB: return "EBU-SUBT";
    default: return "";
    }
}

int player_components(player *b, player_component *out, int max) {
    int n = 0;
    pthread_mutex_lock(&b->lock);
    for (int i = 0; b->have_pmt && i < b->cur_pmt.n && n < max; i++) {
        const ts_es *e = &b->cur_pmt.es[i];
        int kind = es_is_video(e->codec)                                ? 0
                   : es_is_audio(e->codec)                              ? 1
                   : e->codec == ES_DVBSUB || e->codec == ES_TEXTSUB ? 2
                                                                        : -1;
        if (kind < 0) continue;
        player_component *c = &out[n++];
        memset(c, 0, sizeof(*c));
        c->index = e->pid;
        c->kind = kind;
        c->tag = e->component_tag;
        snprintf(c->encoding, sizeof(c->encoding), "%s", oipf_encoding(e->codec));
        snprintf(c->lang, sizeof(c->lang), "%s", e->lang);
        c->audio_type = e->audio_type;
        c->channels = e->channels;
        c->hearing = e->codec == ES_DVBSUB ? e->sub_type >= 0x20 && e->sub_type <= 0x25 : e->sub_type == 5;
        c->ttx_page = e->codec == ES_TEXTSUB ? e->ttx_page : 0;
        c->composition = e->codec == ES_DVBSUB ? e->composition : 0;
        c->active = kind == 0 || (kind == 1 && e->pid == b->cur_apid);
    }
    pthread_mutex_unlock(&b->lock);
    return n;
}

int player_dvb_ids(player *b, int *onid, int *tsid, int *sid) {
    pthread_mutex_lock(&b->lock);
    *onid = b->dvb_onid;
    *tsid = b->dvb_tsid;
    *sid = b->dvb_sid;
    pthread_mutex_unlock(&b->lock);
    return *onid >= 0 && *tsid >= 0 && *sid > 0;
}

void player_status_get(player *b, player_status *st) {
    memset(st, 0, sizeof(*st));
    pthread_mutex_lock(&b->lock);
    st->video_w = b->video_w;
    st->video_h = b->video_h;
    st->video_fps = b->video_fps;
    st->interlaced = b->interlaced;
    pthread_mutex_unlock(&b->lock);
    st->active = b->thread_running && b->state != ST_DONE;
    st->first_frame = b->t_first_frame != 0;
    st->first_frame_ms = b->t_first_frame ? (b->t_first_frame - b->s0) / 1e6 : 0;
    st->failed = b->state == ST_DONE && !b->t_first_frame;
    st->ended = b->thread_running && (b->feeder_eof || b->state == ST_DONE);
    st->http_status = b->http_status;
    st->http_wait_ms = st->active && !b->t_http ? (int) ((mt_now_ns() - b->s0) / NS_PER_MS) : 0;
    /* picture watchdog: since the last frame, or since the first data or
     * resuming after a pause; not for radio, pause, media URLs */
    pthread_mutex_lock(&b->lock);
    int64_t ref = b->last_frame_at;
    int radio = b->have_pmt && b->vpid < 0;
    pthread_mutex_unlock(&b->lock);
    if (b->t_first_byte > ref) ref = b->t_first_byte;
    if (b->resumed_at > ref) ref = b->resumed_at;
    st->picture_age_ms = st->active && !b->paused && !b->media && !radio && ref
                             ? (int) ((mt_now_ns() - ref) / NS_PER_MS)
                             : -1;
    snprintf(st->vcodec, sizeof(st->vcodec), "%s", es_codec_name(b->vcodec));
    snprintf(st->acodec, sizeof(st->acodec), "%s", b->apid >= 0 ? es_codec_name(b->acodec) : "-");
    st->audio_channels = b->have_ainfo ? b->ainfo.channels : 0;
}

void player_tick(player *b) {
    int64_t now = mt_now_ns();

    pthread_mutex_lock(&b->lock);
    int geo = b->geo_pending, vw = b->src_w > 0 ? b->src_w : b->video_w, vh = b->src_w > 0 ? b->src_h : b->video_h;
    SDL_Rect dst = {0, 0, 1920, 1080};
    if (b->dst_w > 0) dst = (SDL_Rect){b->dst_x, b->dst_y, b->dst_w, b->dst_h};
    b->geo_pending = 0;
    char wid[128];
    snprintf(wid, sizeof(wid), "%s", b->window_id);
    pthread_mutex_unlock(&b->lock);
    if (geo && wid[0] && set_exported_window) {
        SDL_Rect src = {0, 0, vw > 0 ? vw : 1920, vh > 0 ? vh : 1080};
        SDL_bool ok = set_exported_window(wid, &src, &dst);
        mt_log("[player] window %s: %dx%d -> %d,%d %dx%d (%s)", wid, src.w, src.h, dst.x, dst.y, dst.w, dst.h,
               ok ? "ok" : SDL_GetError());
    }

    if (b->stats_ms > 0 && b->thread_running && now - b->last_stats_at >= (int64_t) b->stats_ms * NS_PER_MS) {
        double dt = (now - b->last_stats_at) / 1e9;
        double mbit = (b->bytes_in - b->last_stats_bytes) * 8 / 1e6 / dt;
        double fps = (b->frames31 - b->last_stats_frames31) / dt; /* 0x31: frames shown */
        b->last_stats_frames31 = b->frames31;
        b->last_stats_at = now;
        b->last_stats_bytes = b->bytes_in;
        b->last_stats_frames = b->frames;
        smp_api *rqa = api_get(b, 1, -1);
        int rq = rqa ? smp_api_render_queue_length(rqa) : -1;
        api_put(b, rqa);
        /* lead: how far the last fed frame is ahead of the last shown one
         * = buffering in the pipeline, in time */
        double lead = (b->last_fed_vpts_ns && b->last_frame_pts) ? (b->last_fed_vpts_ns - b->last_frame_pts) / 1e6 : -1;
        mt_log("[stat] %.1f Mbit/s, V %llu (%.1f MB) A %llu, shown %llu (%.1f fps), dropped %llu, "
               "lead %.0f ms, RenderQ %d, BufferFull retries %llu, feed errors %llu, CC %llu/%llu, noPTS %llu, "
               "PAT %llu (%d prog.), PMT %llu, Sync %llu",
               mbit, (unsigned long long) b->v_fed, b->v_bytes / 1e6, (unsigned long long) b->a_fed,
               (unsigned long long) b->frames31, fps, (unsigned long long) b->dropped_frames, lead, rq,
               (unsigned long long) b->full_retries, (unsigned long long) b->feed_errors,
               (unsigned long long) b->pes_cc_err, (unsigned long long) b->ts.cc_errors,
               (unsigned long long) b->no_pts, (unsigned long long) b->ts.pat_seen, b->ts.nprog,
               (unsigned long long) b->ts.pmt_seen, (unsigned long long) b->ts.sync_losses);
    }
}

void player_stop(player *b) {
    if (!b) return;
    mt_log("[player] stop");
    stop_feeder(b);
    pthread_mutex_lock(&b->lock);
    smp_api *api = b->api;
    cbctx *ctx = b->api_ctx;
    int loaded = b->loaded;
    b->api = NULL;
    b->api_ctx = NULL;
    b->loaded = 0;
    __atomic_add_fetch(&b->gen, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&b->lock);
    api_drain(b);
    unload_api(b, api, ctx, loaded);
    smp_api_destroy(api);
    free(ctx);
    mpa_pcm_free(b->mpa);
    b->mpa = NULL;
    b->state = ST_DONE;
}

void player_destroy(player *b) {
    if (!b) return;
    player_stop(b);
    pthread_mutex_destroy(&b->lock);
    pthread_cond_destroy(&b->cond);
    free(b);
}
