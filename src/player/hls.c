/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hls.h"

#include "log.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define MAX_TEXT (4u << 20) /* largest playlist */
#define LIVE_BACK 3         /* live: start this many segments before the end */
#define MAX_HEIGHT 1080     /* variant up to Full HD (decoder, bandwidth) */

typedef struct {
    double dur;
    char *uri; /* absolute */
} seg;

struct hls {
    const http_hook *hook;
    volatile int *stop;
    char media[4096]; /* media playlist (absolute, after redirects) */
    seg *segs;
    int nseg;
    long long seq0;     /* media sequence number of segs[0] */
    double target;      /* EXT-X-TARGETDURATION */
    int endlist;        /* VOD or a finished live broadcast */
    double total, pos0; /* VOD duration, time at the first delivered segment */
    long long next;     /* next segment to deliver */
    http_conn cur;
    int cur_open;
    double reloaded_at;
    int fails;
};

static double now_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double) t.tv_sec + t.tv_nsec / 1e9;
}

/* unregister with the owner (http_open_ex registers successful sockets) */
static void unhook(hls *h) {
    if (h->hook && h->hook->fd) h->hook->fd(h->hook->user, -1);
}

static char *fetch_text(hls *h, const char *url, char *final_url, size_t fl) {
    http_conn c;
    if (http_open_ex(url, NULL, NULL, -1, h->hook, &c) != 0) return NULL;
    if (final_url) snprintf(final_url, fl, "%s", c.url[0] ? c.url : url);
    size_t cap = 64 * 1024, len = 0;
    char *b = malloc(cap);
    while (b) {
        if (len + 4096 > cap) {
            if (cap >= MAX_TEXT) break;
            char *n = realloc(b, cap * 2);
            if (!n) {
                free(b);
                b = NULL;
                break;
            }
            b = n;
            cap *= 2;
        }
        ssize_t r = http_read(&c, b + len, cap - len - 1);
        if (r <= 0) break;
        len += (size_t) r;
    }
    unhook(h);
    http_close(&c);
    if (b) b[len] = 0;
    return b;
}

/* value of an attribute (KEY=value or KEY="value") from a tag line */
static int attr(const char *line, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *p = strchr(line, ':'); p && *p; p++) {
        if ((*p == ':' || *p == ',') && !strncmp(p + 1, key, kl) && p[1 + kl] == '=') {
            const char *v = p + 2 + kl;
            size_t i = 0;
            if (*v == '"') {
                v++;
                while (*v && *v != '"' && i + 1 < n) out[i++] = *v++;
            } else {
                while (*v && *v != ',' && *v != '\r' && *v != '\n' && i + 1 < n) out[i++] = *v++;
            }
            out[i] = 0;
            return 1;
        }
    }
    return 0;
}

/* next line (without CR/LF) - splits text in place */
static char *next_line(char **p) {
    char *s = *p;
    if (!s || !*s) return NULL;
    char *e = strchr(s, '\n');
    if (e) {
        *e = 0;
        *p = e + 1;
    } else {
        *p = s + strlen(s);
    }
    size_t l = strlen(s);
    while (l && (s[l - 1] == '\r' || s[l - 1] == ' ')) s[--l] = 0;
    return s;
}

/* master: best variant up to MAX_HEIGHT whose audio is embedded in the stream */
static int pick_variant(char *text, const char *base, char *out, size_t n, char *err, size_t el) {
    /* audio groups with their own playlist (URI) */
    char sep_groups[16][64];
    int nsep = 0;
    char *copy = strdup(text), *p = copy, *l;
    while (copy && (l = next_line(&p))) {
        char type[16], gid[64], uri[8];
        if (!strncmp(l, "#EXT-X-MEDIA:", 13) && attr(l, "TYPE", type, sizeof(type)) && !strcmp(type, "AUDIO") &&
            attr(l, "GROUP-ID", gid, sizeof(gid)) && attr(l, "URI", uri, sizeof(uri)) && nsep < 16)
            snprintf(sep_groups[nsep++], sizeof(sep_groups[0]), "%s", gid);
    }
    free(copy);
    long best_bw = -1, any_bw = -1;
    char best[4096] = "", any[4096] = "";
    p = text;
    while ((l = next_line(&p))) {
        if (strncmp(l, "#EXT-X-STREAM-INF:", 18)) continue;
        char bw_s[32] = "0", res[32] = "", aud[64] = "";
        attr(l, "BANDWIDTH", bw_s, sizeof(bw_s));
        attr(l, "RESOLUTION", res, sizeof(res));
        attr(l, "AUDIO", aud, sizeof(aud));
        char *u = next_line(&p);
        while (u && (!*u || *u == '#')) u = next_line(&p);
        if (!u) break;
        long bw = atol(bw_s);
        int h = 0;
        const char *x = strchr(res, 'x');
        if (x) h = atoi(x + 1);
        int separate = 0;
        for (int i = 0; i < nsep && aud[0]; i++)
            if (!strcmp(sep_groups[i], aud)) separate = 1;
        if (separate) continue;
        if (bw > any_bw) {
            any_bw = bw;
            snprintf(any, sizeof(any), "%s", u);
        }
        if ((h == 0 || h <= MAX_HEIGHT) && bw > best_bw) {
            best_bw = bw;
            snprintf(best, sizeof(best), "%s", u);
        }
    }
    const char *v = best[0] ? best : any;
    if (!v[0]) {
        snprintf(err, el, "no variant with embedded audio");
        return -1;
    }
    http_join_url(base, v, out, n);
    mt_log("[hls] variant %ld bit/s: %.200s", best[0] ? best_bw : any_bw, out);
    return 0;
}

static void free_segs(hls *h) {
    for (int i = 0; i < h->nseg; i++) free(h->segs[i].uri);
    free(h->segs);
    h->segs = NULL;
    h->nseg = 0;
}

static int parse_media(hls *h, char *text, char *err, size_t el) {
    free_segs(h);
    int cap = 0;
    double d = -1;
    long long seq0 = 0;
    char *p = text, *l;
    h->endlist = 0;
    while ((l = next_line(&p))) {
        if (!strncmp(l, "#EXT-X-TARGETDURATION:", 22)) h->target = atof(l + 22);
        else if (!strncmp(l, "#EXT-X-MEDIA-SEQUENCE:", 22)) seq0 = atoll(l + 22);
        else if (!strncmp(l, "#EXT-X-ENDLIST", 14)) h->endlist = 1;
        else if (!strncmp(l, "#EXT-X-MAP:", 11)) {
            snprintf(err, el, "fMP4 segments (EXT-X-MAP)");
            return -1;
        } else if (!strncmp(l, "#EXT-X-KEY:", 11)) {
            char m[32] = "";
            attr(l, "METHOD", m, sizeof(m));
            if (strcmp(m, "NONE") != 0) {
                snprintf(err, el, "encrypted (%s)", m);
                return -1;
            }
        } else if (!strncmp(l, "#EXTINF:", 8)) d = atof(l + 8);
        else if (*l && *l != '#') {
            if (h->nseg == cap) {
                cap = cap ? cap * 2 : 64;
                seg *n = realloc(h->segs, (size_t) cap * sizeof(seg));
                if (!n) return -1;
                h->segs = n;
            }
            char abs[4096];
            http_join_url(h->media, l, abs, sizeof(abs));
            h->segs[h->nseg].dur = d > 0 ? d : h->target;
            h->segs[h->nseg].uri = strdup(abs);
            h->nseg++;
            d = -1;
        }
    }
    h->seq0 = seq0;
    if (h->target <= 0) h->target = 4;
    if (!h->nseg) {
        snprintf(err, el, "playlist without segments");
        return -1;
    }
    return 0;
}

hls *hls_open(const char *url, double start, const http_hook *hook, volatile int *stop, char *err, size_t errlen) {
    hls *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->hook = hook;
    h->stop = stop;
    h->cur.fd = -1;
    char final[4096];
    char *t = fetch_text(h, url, final, sizeof(final));
    if (!t || strncmp(t, "#EXTM3U", 7) != 0) {
        snprintf(err, errlen, "%s", t ? "not an HLS playlist" : "cannot fetch playlist");
        free(t);
        free(h);
        return NULL;
    }
    if (strstr(t, "#EXT-X-STREAM-INF")) {
        if (pick_variant(t, final, h->media, sizeof(h->media), err, errlen) != 0) {
            free(t);
            free(h);
            return NULL;
        }
        free(t);
        t = fetch_text(h, h->media, h->media, sizeof(h->media));
        if (!t) {
            snprintf(err, errlen, "cannot fetch media playlist");
            free(h);
            return NULL;
        }
    } else {
        snprintf(h->media, sizeof(h->media), "%s", final);
    }
    int rc = parse_media(h, t, err, errlen);
    free(t);
    if (rc != 0) {
        hls_close(h);
        return NULL;
    }
    h->reloaded_at = now_s();
    if (h->endlist) {
        int i = 0;
        double cum = 0;
        for (int k = 0; k < h->nseg; k++) h->total += h->segs[k].dur;
        while (i < h->nseg - 1 && cum + h->segs[i].dur <= start) cum += h->segs[i++].dur;
        h->pos0 = cum;
        h->next = h->seq0 + i;
    } else {
        h->next = h->seq0 + (h->nseg > LIVE_BACK ? h->nseg - LIVE_BACK : 0);
    }
    mt_log("[hls] %s, %d segments (target %.0f s)%s, starting at segment %lld (%.1f s)", h->endlist ? "VOD" : "live",
           h->nseg, h->target, h->endlist ? "" : "", h->next, h->pos0);
    return h;
}

static int reload(hls *h) {
    char err[96];
    char *t = fetch_text(h, h->media, NULL, 0);
    if (!t) return -1;
    int rc = parse_media(h, t, err, sizeof(err));
    free(t);
    h->reloaded_at = now_s();
    if (rc != 0) mt_log("[hls] reload: %s", err);
    return rc;
}

ssize_t hls_read(hls *h, void *buf, size_t n) {
    for (;;) {
        if (*h->stop) return -1;
        if (h->cur_open) {
            ssize_t r = http_read(&h->cur, buf, n);
            if (r > 0) return r;
            unhook(h);
            http_close(&h->cur);
            h->cur_open = 0;
            if (r < 0 && !*h->stop) mt_log("[hls] segment %lld aborted", h->next);
            h->next++;
            continue;
        }
        long long idx = h->next - h->seq0;
        if (idx < 0) { /* too slow: the window has moved on */
            mt_log("[hls] segment %lld no longer in playlist - continuing at %lld", h->next, h->seq0);
            h->next = h->seq0;
            continue;
        }
        if (idx < h->nseg) {
            if (http_open_ex(h->segs[idx].uri, NULL, NULL, -1, h->hook, &h->cur) == 0) {
                h->cur_open = 1;
                h->fails = 0;
            } else if (!*h->stop) {
                mt_log("[hls] cannot fetch segment %lld (status %d)", h->next, h->cur.status);
                if (++h->fails >= 5) return -1;
                h->next++; /* try the next one */
            }
            continue;
        }
        if (h->endlist) return 0;
        /* live: reload the playlist, no more often than every half target duration */
        double wait = h->target / 2 - (now_s() - h->reloaded_at);
        while (wait > 0 && !*h->stop) {
            usleep(wait > 0.1 ? 100000 : (useconds_t) (wait * 1e6));
            wait -= 0.1;
        }
        if (*h->stop) return -1;
        if (reload(h) != 0 && ++h->fails >= 5) return -1;
    }
}

double hls_duration(const hls *h) { return h->endlist ? h->total : 0; }
double hls_start_pos(const hls *h) { return h->pos0; }
int hls_is_live(const hls *h) { return !h->endlist; }

void hls_close(hls *h) {
    if (!h) return;
    if (h->cur_open) {
        unhook(h);
        http_close(&h->cur);
    }
    free_segs(h);
    free(h);
}
