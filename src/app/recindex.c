/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "recindex.h"

#include "http.h"
#include "log.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#define TS 188
#define INDEX_ANCHORS 128       /* samples across the file */
#define PROBE_MAX (768 * 1024)  /* search this far for a PCR */
#define TAIL_SPAN (1024 * 1024) /* end: last PCR in this range */
#define PCR_WRAP (8589934592.0 / 90000.0)

typedef struct {
    long long off;
    double t;
} anchor;

struct recindex {
    pthread_mutex_t lock;
    int refs;
    int state;
    char url[1024];
    anchor *a; /* sorted by off */
    int n;
    double dur;
    int phase; /* position of the packet boundaries (offset mod 188) */
};

static void unref(recindex *x) {
    pthread_mutex_lock(&x->lock);
    int left = --x->refs;
    pthread_mutex_unlock(&x->lock);
    if (left) return;
    free(x->a);
    pthread_mutex_destroy(&x->lock);
    free(x);
}

static int abandoned(recindex *x) {
    pthread_mutex_lock(&x->lock);
    int r = x->refs < 2;
    pthread_mutex_unlock(&x->lock);
    return r;
}

static double pcr_of(const unsigned char *p) {
    unsigned afc = (p[3] >> 4) & 3;
    if (!(afc & 2) || p[4] < 7 || !(p[5] & 0x10)) return -1;
    const unsigned char *b = p + 6;
    uint64_t base = ((uint64_t) b[0] << 25) | ((uint64_t) b[1] << 17) | ((uint64_t) b[2] << 9) |
                    ((uint64_t) b[3] << 1) | (uint64_t) (b[4] >> 7);
    return (double) base / 90000.0;
}

/* Read range [from, to] and look for PCRs of *pid (< 0: take the first PID with
 * a PCR). last = 0: the first PCR is enough (request ends there), 1: the
 * last one in the range. 0 = found: file offset of the packet and PCR (s);
 * *total = file size according to Content-Range. */
static int probe(recindex *x, long long from, long long to, int last, int *pid, long long *at, double *pcr,
                 long long *total) {
    http_conn c;
    if (http_open_span(x->url, from, to, &c) != 0) return -1;
    if (total && c.total_size > 0) *total = c.total_size;
    size_t cap = (size_t) (to - from + 1);
    unsigned char *buf = malloc(cap);
    size_t have = 0, pos = 0;
    int synced = 0, found = -1;
    while (buf && have < cap && !(found == 0 && !last)) {
        ssize_t r = http_read(&c, buf + have, cap - have);
        if (r <= 0) break;
        have += (size_t) r;
        for (;;) {
            if (!synced) { /* three sync bytes on the 188-byte grid */
                while (pos + 2 * TS < have && !(buf[pos] == 0x47 && buf[pos + TS] == 0x47 && buf[pos + 2 * TS] == 0x47))
                    pos++;
                if (pos + 2 * TS >= have) break; /* more data needed */
                synced = 1;
            }
            if (pos + TS > have) break;
            const unsigned char *p = buf + pos;
            if (p[0] != 0x47) { /* lost sync: search again */
                synced = 0;
                continue;
            }
            int ppid = ((p[1] & 0x1f) << 8) | p[2];
            double v = *pid >= 0 && ppid != *pid ? -1 : pcr_of(p);
            if (v >= 0) {
                *pid = ppid;
                *at = from + (long long) pos;
                *pcr = v;
                found = 0;
                if (!last) break;
            }
            pos += TS;
        }
    }
    free(buf);
    http_close(&c);
    return found;
}

static void add_anchor(recindex *x, long long off, double t) {
    pthread_mutex_lock(&x->lock);
    int i = 0;
    while (i < x->n && x->a[i].off < off) i++;
    /* only if the time fits the neighbours (skip jumps in the stream) */
    int ok = (i == 0 || x->a[i - 1].t <= t) && (i == x->n || (t <= x->a[i].t && x->a[i].off != off));
    if (ok) {
        memmove(x->a + i + 1, x->a + i, (size_t) (x->n - i) * sizeof(anchor));
        x->a[i] = (anchor){off, t};
        x->n++;
    }
    pthread_mutex_unlock(&x->lock);
}

static double ms_since(const struct timeval *t0) {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (double) (t.tv_sec - t0->tv_sec) * 1000.0 + (double) (t.tv_usec - t0->tv_usec) / 1000.0;
}

static void *load_thread(void *arg) {
    recindex *x = arg;
    struct timeval t0;
    gettimeofday(&t0, NULL);
    char logurl[1024];
    http_url_redact(x->url, logurl, sizeof(logurl));
    int pid = -1;
    long long at = 0, size = -1, tail_at = 0;
    double head = 0, tail = 0, pcr = 0;
    int ok = probe(x, 0, PROBE_MAX - 1, 0, &pid, &at, &head, &size) == 0 && size > PROBE_MAX;
    if (ok) {
        long long from = size - 1 - TAIL_SPAN;
        if (from < at + TS) from = at + TS;
        /* up to size - 2: never the last byte (otherwise "played") */
        ok = probe(x, from, size - 2, 1, &pid, &tail_at, &tail, NULL) == 0;
    }
    double dur = tail - head;
    if (dur < 0) dur += PCR_WRAP;
    pthread_mutex_lock(&x->lock);
    x->a = ok ? calloc(INDEX_ANCHORS + 2, sizeof(anchor)) : NULL;
    if (x->a) {
        x->a[0] = (anchor){at, 0};
        x->a[1] = (anchor){tail_at, dur};
        x->n = 2;
        x->dur = dur;
        x->phase = (int) (at % TS);
    }
    x->state = x->a ? 1 : -1;
    pthread_mutex_unlock(&x->lock);
    if (!x->a) {
        mt_log("[recindex] no PCR in %s", logurl);
        unref(x);
        return NULL;
    }
    mt_log("[recindex] %.0f s, %lld bytes, PCR PID %d (%.0f ms)", dur, size, pid, ms_since(&t0));
    /* finer: points in coarse -> fine order (bit reversal) */
    int bits = 0;
    while ((1 << bits) < INDEX_ANCHORS) bits++;
    for (int k = 1; k < (1 << bits); k++) {
        if (abandoned(x)) break;
        int r = 0;
        for (int b = 0; b < bits; b++)
            if (k & (1 << b)) r |= 1 << (bits - 1 - b);
        long long off = at + (long long) ((double) (tail_at - at) * r / (1 << bits));
        off -= (off - x->phase) % TS;
        if (off <= at || off + PROBE_MAX > tail_at) continue;
        long long pat = 0;
        if (probe(x, off, off + PROBE_MAX - 1, 0, &pid, &pat, &pcr, NULL) != 0) continue;
        double t = pcr - head;
        if (t < 0) t += PCR_WRAP;
        add_anchor(x, pat, t);
    }
    pthread_mutex_lock(&x->lock);
    int n = x->n;
    pthread_mutex_unlock(&x->lock);
    mt_log("[recindex] %d anchors after %.0f ms", n, ms_since(&t0));
    unref(x);
    return NULL;
}

recindex *recindex_fetch(const char *url) {
    recindex *x = calloc(1, sizeof(*x));
    if (!x) return NULL;
    pthread_mutex_init(&x->lock, NULL);
    snprintf(x->url, sizeof(x->url), "%s", url);
    x->refs = 2; /* caller + loader thread */
    pthread_t th;
    if (pthread_create(&th, NULL, load_thread, x) != 0) {
        x->state = -1;
        x->refs = 1;
        return x;
    }
    pthread_detach(th);
    return x;
}

int recindex_state(recindex *x) {
    if (!x) return -1;
    pthread_mutex_lock(&x->lock);
    int s = x->state;
    pthread_mutex_unlock(&x->lock);
    return s;
}

double recindex_duration(recindex *x) {
    if (!x) return 0;
    pthread_mutex_lock(&x->lock);
    double d = x->state == 1 ? x->dur : 0;
    pthread_mutex_unlock(&x->lock);
    return d;
}

int recindex_lookup(recindex *x, double t, long long *offset, double *seg_t) {
    if (!x) return -1;
    pthread_mutex_lock(&x->lock);
    if (x->state != 1 || x->n < 2) {
        pthread_mutex_unlock(&x->lock);
        return -1;
    }
    if (t < 0) t = 0;
    int i = 0; /* last point with time <= t, at most the second to last */
    while (i + 2 < x->n && x->a[i + 1].t <= t) i++;
    const anchor *a = &x->a[i], *b = &x->a[i + 1];
    double f = b->t > a->t ? (t - a->t) / (b->t - a->t) : 0;
    if (f > 1) f = 1;
    long long off = a->off + (long long) ((double) (b->off - a->off) * f);
    off -= (off - x->phase) % TS;
    if (off < a->off) off = a->off;
    *offset = off;
    *seg_t = b->off > a->off ? a->t + (double) (off - a->off) / (double) (b->off - a->off) * (b->t - a->t) : a->t;
    pthread_mutex_unlock(&x->lock);
    return 0;
}

void recindex_free(recindex *x) {
    if (x) unref(x);
}
