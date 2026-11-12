/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tvhstat.h"

#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static struct {
    pthread_mutex_t lock;
    uint32_t want;
    tvhstat_info info;
    int changed;
    int logged; /* per subscription: 1 signalStatus, 2 descrambleInfo already logged */
    /* "No access" may come before subscriptionStart (tvh holds the start for a
     * second): kept apart from info, which the start clears */
    uint32_t noacc_channel;
    int noacc;
} T = {PTHREAD_MUTEX_INITIALIZER, 0, {0}, 0, 0, 0, 0};

static const char *js(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return cJSON_IsString(v) ? v->valuestring : "";
}

static int jhas(const cJSON *o, const char *k, double *out) {
    const cJSON *v = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    if (!cJSON_IsNumber(v)) return 0;
    *out = v->valuedouble;
    return 1;
}

/* CA systems of the CA streams (tvheadend fork: caid, caname), each CAID once */
static void ca_list(tvhstat_info *i, const cJSON *j) {
    const cJSON *s;
    unsigned seen[8];
    int n = 0;
    size_t o = 0;
    i->ca_list[0] = 0;
    cJSON_ArrayForEach(s, cJSON_GetObjectItemCaseSensitive(j, "streams")) {
        double v;
        if (strcmp(js(s, "type"), "CA") || !jhas(s, "caid", &v) || n >= 8) continue;
        unsigned caid = (unsigned) v;
        int dup = 0;
        for (int k = 0; k < n; k++) dup |= seen[k] == caid;
        if (dup) continue;
        seen[n++] = caid;
        const char *name = js(s, "caname");
        /* caid2name gives the hex number for an unknown system */
        int named = name[0] && strncasecmp(name, "0x", 2) && strcasecmp(name, "unknown");
        if (o < sizeof(i->ca_list))
            o += (size_t) snprintf(i->ca_list + o, sizeof(i->ca_list) - o, "%s%s%s%04X", o ? ", " : "",
                                   named ? name : "", named ? " " : "", caid);
    }
}

static void on_start(tvhstat_info *i, const cJSON *j) {
    const cJSON *si = cJSON_GetObjectItemCaseSensitive(j, "sourceinfo");
    i->started = 1;
    ca_list(i, j);
    i->error[0] = 0;
    snprintf(i->provider, sizeof(i->provider), "%s", js(si, "provider"));
    snprintf(i->service, sizeof(i->service), "%s", js(si, "service"));
    snprintf(i->adapter, sizeof(i->adapter), "%s", js(si, "adapter"));
    snprintf(i->mux, sizeof(i->mux), "%s", js(si, "mux"));
    snprintf(i->network, sizeof(i->network), "%s", js(si, "network"));
}

static void on_signal(tvhstat_info *i, const cJSON *j) {
    double v;
    snprintf(i->status, sizeof(i->status), "%s", js(j, "feStatus"));
    if (jhas(j, "feAbsoluteSNR", &v)) i->has_snr = 1, i->snr_abs = 1, i->snr = (int64_t) v;
    else if (jhas(j, "feSNR", &v)) i->has_snr = 1, i->snr_abs = 0, i->snr = (int64_t) v;
    else i->has_snr = 0;
    if (jhas(j, "feAbsoluteSignal", &v)) i->has_signal = 1, i->signal_abs = 1, i->signal = (int64_t) v;
    else if (jhas(j, "feSignal", &v)) i->has_signal = 1, i->signal_abs = 0, i->signal = (int64_t) v;
    else i->has_signal = 0;
    i->has_ber = jhas(j, "feBER", &v);
    i->ber = i->has_ber ? (uint32_t) v : 0;
    i->has_unc = jhas(j, "feUNC", &v);
    i->unc = i->has_unc ? (uint32_t) v : 0;
}

static void on_descramble(tvhstat_info *i, const cJSON *j) {
    double v;
    i->has_descr = 1;
    i->caid = jhas(j, "caid", &v) ? (uint32_t) v : 0;
    i->provid = jhas(j, "provid", &v) ? (uint32_t) v : 0;
    i->ecmtime = jhas(j, "ecmtime", &v) ? (uint32_t) v : 0;
    i->hops = jhas(j, "hops", &v) ? (uint32_t) v : 0;
    snprintf(i->cardsystem, sizeof(i->cardsystem), "%s", js(j, "cardsystem"));
    snprintf(i->reader, sizeof(i->reader), "%s", js(j, "reader"));
    snprintf(i->from, sizeof(i->from), "%s", js(j, "from"));
    snprintf(i->protocol, sizeof(i->protocol), "%s", js(j, "protocol"));
}

static void on_error(tvhstat_info *i, const cJSON *j) {
    const char *e = js(j, "subscriptionError");
    snprintf(i->error, sizeof(i->error), "%s", e[0] ? e : js(j, "status"));
}

static void log_once(int bit, const char *method, const cJSON *j) {
    if (T.logged & bit) return;
    T.logged |= bit;
    char *t = cJSON_PrintUnformatted(j);
    mt_log("[tvhstat] %s %s", method, t ? t : "");
    free(t);
}

void tvhstat_feed(unsigned long channel, const char *method, const cJSON *j) {
    if (!method || !j) return;
    pthread_mutex_lock(&T.lock);
    if (!strcmp(method, "subscriptionStart")) {
        /* a new start of the same channel (tvh restarts a service whose
         * descrambler gives up, every few seconds) keeps the tuner's values:
         * they would otherwise only come back shortly before the next start */
        tvhstat_info keep = T.info;
        memset(&T.info, 0, sizeof(T.info));
        if (keep.channel_id == (uint32_t) channel) {
            memcpy(T.info.status, keep.status, sizeof(T.info.status));
            T.info.has_snr = keep.has_snr, T.info.snr_abs = keep.snr_abs, T.info.snr = keep.snr;
            T.info.has_signal = keep.has_signal, T.info.signal_abs = keep.signal_abs, T.info.signal = keep.signal;
            T.info.has_ber = keep.has_ber, T.info.ber = keep.ber;
            T.info.has_unc = keep.has_unc, T.info.unc = keep.unc;
        }
        T.info.channel_id = (uint32_t) channel;
        T.logged = 0;
        on_start(&T.info, j);
        if (T.noacc_channel != (uint32_t) channel) T.noacc = 0;
        T.info.no_access = T.noacc;
        T.changed = 1;
    } else if (!strcmp(method, "subscriptionStatus") && (uint32_t) channel != T.info.channel_id) {
        /* before its start: only whether it is scrambled without a key */
        T.noacc_channel = (uint32_t) channel;
        T.noacc = !strcmp(js(j, "subscriptionError"), "scrambled");
        if (T.noacc) mt_log("[tvhstat] channel %lu: no access (before the start)", channel);
    } else if ((uint32_t) channel != T.info.channel_id) {
        /* message from another (old) subscription */
    } else if (!strcmp(method, "signalStatus")) {
        on_signal(&T.info, j);
        log_once(1, method, j);
        T.changed = 1;
    } else if (!strcmp(method, "descrambleInfo")) {
        on_descramble(&T.info, j);
        log_once(2, method, j);
        T.noacc = T.info.no_access = 0; /* a key came */
        T.changed = 1;
    } else if (!strcmp(method, "subscriptionStatus")) {
        int noacc = !strcmp(js(j, "subscriptionError"), "scrambled"), flip = noacc != T.info.no_access;
        if (flip) T.changed = 1;
        T.noacc_channel = (uint32_t) channel;
        T.noacc = T.info.no_access = noacc;
        if (noacc) {
            if (flip) mt_log("[tvhstat] channel %lu: no access", channel);
            T.info.error[0] = 0; /* shown as the failed decryption, not as tvh's text */
        } else if (js(j, "status")[0] || js(j, "subscriptionError")[0]) {
            mt_log("[tvhstat] subscriptionStatus %s %s", js(j, "status"), js(j, "subscriptionError"));
            on_error(&T.info, j);
            T.changed = 1;
        } else if (T.info.error[0]) {
            T.info.error[0] = 0; /* empty status: all is well again */
            T.changed = 1;
        }
    } else if (!strcmp(method, "subscriptionStop")) {
        on_error(&T.info, j);
        T.changed = 1;
    }
    pthread_mutex_unlock(&T.lock);
}

int tvhstat_no_access(uint32_t channel_id, tvhstat_info *out) {
    pthread_mutex_lock(&T.lock);
    int r = channel_id && T.noacc && T.noacc_channel == channel_id;
    if (r && out) {
        if (T.info.channel_id == channel_id) *out = T.info;
        else {
            memset(out, 0, sizeof(*out));
            out->channel_id = channel_id;
        }
        out->no_access = 1;
    }
    pthread_mutex_unlock(&T.lock);
    return r;
}

void tvhstat_watch(uint32_t channel_id) {
    pthread_mutex_lock(&T.lock);
    if (T.want != channel_id) {
        T.want = channel_id;
        T.changed = 1;
    }
    pthread_mutex_unlock(&T.lock);
}

int tvhstat_get(tvhstat_info *out) {
    pthread_mutex_lock(&T.lock);
    int c = T.changed;
    T.changed = 0;
    if (T.want && T.info.channel_id == T.want) *out = T.info;
    else memset(out, 0, sizeof(*out));
    pthread_mutex_unlock(&T.lock);
    return c;
}
