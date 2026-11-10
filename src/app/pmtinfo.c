/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pmtinfo.h"

#include "http.h"
#include "log.h"
#include "ts.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_CHANNELS 64
#define READ_LIMIT (4 * 1024 * 1024) /* PMT comes right at the start */
#define TIME_LIMIT_S 5
#define SDT_WAIT_MS 2500 /* after the PMT: the SDT (original network id) comes every 2 s at most */

typedef struct {
    unsigned long channel;
    int state; /* 0 = free, 1 = in progress, 2 = known, 3 = failed */
    ts_pmt pmt;
} entry;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static entry g_tab[MAX_CHANNELS];
static int g_next; /* next slot when all are taken (oldest goes) */
static char g_host[64], g_user[64], g_pass[64];
static int g_port = 9981;
static pmtinfo_cb g_cb;

void pmtinfo_init(const char *host, int http_port, const char *user, const char *pass) {
    snprintf(g_host, sizeof(g_host), "%s", host ? host : "");
    snprintf(g_user, sizeof(g_user), "%s", user ? user : "");
    snprintf(g_pass, sizeof(g_pass), "%s", pass ? pass : "");
    if (http_port > 0) g_port = http_port;
}

void pmtinfo_set_callback(pmtinfo_cb cb) { g_cb = cb; }

static entry *find(unsigned long channel) {
    for (int i = 0; i < MAX_CHANNELS; i++)
        if (g_tab[i].state && g_tab[i].channel == channel) return &g_tab[i];
    return NULL;
}

/* ---------------- PMT of the HTTP stream ---------------- */

typedef struct {
    ts_pmt pmt;
    int done;
} reader;

static void on_pmt(void *user, const ts_pmt *pmt) {
    reader *r = user;
    r->pmt = *pmt;
    r->done = 1;
}

static void on_pes(void *user, int pid, const uint8_t *data, size_t len, int64_t pts, int64_t dts, int rai,
                   int cc_err) {
    (void) user, (void) pid, (void) data, (void) len, (void) pts, (void) dts, (void) rai, (void) cc_err;
}

/* the PAT/PMT with the demuxer that plays the stream: one programme (pass profile) */
static int read_pmt(unsigned long channel, reader *r) {
    char url[256];
    snprintf(url, sizeof(url), "http://%s:%d/stream/channelid/%lu?profile=pass", g_host, g_port, channel);
    http_conn c;
    if (http_open_quiet(url, g_user, g_pass, &c) != 0) return -1;
    ts_demux *d = calloc(1, sizeof(*d));
    if (!d) {
        http_close(&c);
        return -1;
    }
    ts_init(d, on_pmt, on_pes, r);
    ts_select(d, -1, -1);
    uint8_t buf[188 * 64];
    size_t total = 0;
    time_t t0 = time(NULL);
    struct timespec pmt_at = {0, 0}, now;
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (r->done && !pmt_at.tv_sec) pmt_at = now;
        /* the PMT, then the SDT for the original network id (HbbTV) */
        if (r->done && (d->onid >= 0 || (now.tv_sec - pmt_at.tv_sec) * 1000 +
                                                (now.tv_nsec - pmt_at.tv_nsec) / 1000000 >= SDT_WAIT_MS))
            break;
        if (total >= READ_LIMIT || time(NULL) - t0 > TIME_LIMIT_S) break;
        ssize_t n = http_read(&c, buf, sizeof(buf));
        if (n <= 0) break;
        total += (size_t) n;
        ts_feed(d, buf, (size_t) n);
    }
    r->pmt.tsid = d->tsid;
    r->pmt.onid = d->onid;
    ts_free(d);
    free(d);
    http_close(&c);
    return r->done ? 0 : -1;
}

static void *lookup_thread(void *arg) {
    unsigned long channel = (unsigned long) (uintptr_t) arg;
    reader *r = calloc(1, sizeof(*r));
    int rc = r ? read_pmt(channel, r) : -1;
    pthread_mutex_lock(&g_lock);
    entry *e = find(channel);
    if (e) {
        e->state = rc == 0 ? 2 : 3;
        if (rc == 0) e->pmt = r->pmt;
    }
    pthread_mutex_unlock(&g_lock);
    if (rc != 0) {
        mt_log("[pmtinfo] channel %lu: no PMT from the HTTP stream", channel);
    } else {
        char list[64] = "";
        size_t l = 0;
        for (int i = 0; i < r->pmt.n_ttxsub && l + 12 < sizeof(list); i++)
            l += (size_t) snprintf(list + l, sizeof(list) - l, "%s%03X %s/%d", i ? ", " : "", r->pmt.ttxsub[i].page,
                                   r->pmt.ttxsub[i].lang, r->pmt.ttxsub[i].type);
        mt_log("[pmtinfo] channel %lu: %d streams, subtitle pages %s, onid %d tsid %d sid %d", channel, r->pmt.n,
               r->pmt.n_ttxsub ? list : "none", r->pmt.onid, r->pmt.tsid, r->pmt.program);
        if (g_cb && e) g_cb(channel, &r->pmt);
    }
    free(r);
    return NULL;
}

void pmtinfo_lookup(unsigned long channel) {
    if (!channel || !g_host[0]) return;
    pthread_mutex_lock(&g_lock);
    entry *e = find(channel);
    if (e && e->state != 3) {
        /* known: tell again (a new session of the same channel has a new stream list) */
        ts_pmt *known = e->state == 2 ? malloc(sizeof(*known)) : NULL;
        if (known) *known = e->pmt;
        pthread_mutex_unlock(&g_lock);
        if (known) {
            if (g_cb) g_cb(channel, known);
            free(known);
        }
        return;
    }
    if (!e) {
        for (int i = 0; i < MAX_CHANNELS && !e; i++)
            if (!g_tab[i].state) e = &g_tab[i];
        if (!e) {
            e = &g_tab[g_next];
            g_next = (g_next + 1) % MAX_CHANNELS;
        }
    }
    e->channel = channel;
    e->state = 1;
    memset(&e->pmt, 0, sizeof(e->pmt));
    pthread_mutex_unlock(&g_lock);
    pthread_t th;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &a, lookup_thread, (void *) (uintptr_t) channel) != 0) {
        pthread_mutex_lock(&g_lock);
        e->state = 3;
        pthread_mutex_unlock(&g_lock);
    }
    pthread_attr_destroy(&a);
}

int pmtinfo_pages(unsigned long channel, pmtinfo_page *pages, int max) {
    pthread_mutex_lock(&g_lock);
    entry *e = find(channel);
    int n = -1;
    if (e && e->state == 2) {
        n = e->pmt.n_ttxsub < max ? e->pmt.n_ttxsub : max;
        for (int i = 0; i < n; i++) {
            pages[i].page = e->pmt.ttxsub[i].page;
            pages[i].type = e->pmt.ttxsub[i].type;
            snprintf(pages[i].lang, sizeof(pages[i].lang), "%s", e->pmt.ttxsub[i].lang);
        }
    }
    pthread_mutex_unlock(&g_lock);
    return n;
}
