/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "tvhstat.h"

#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    pthread_mutex_t lock;
    uint32_t want;
    tvhstat_info info;
    int changed;
    int logged; /* per subscription: 1 signalStatus, 2 descrambleInfo already logged */
} T = {PTHREAD_MUTEX_INITIALIZER, 0, {0}, 0, 0};

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

static void on_start(tvhstat_info *i, const cJSON *j) {
    const cJSON *si = cJSON_GetObjectItemCaseSensitive(j, "sourceinfo");
    i->started = 1;
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
        memset(&T.info, 0, sizeof(T.info));
        T.info.channel_id = (uint32_t) channel;
        T.logged = 0;
        on_start(&T.info, j);
        T.changed = 1;
    } else if ((uint32_t) channel != T.info.channel_id) {
        /* message from another (old) subscription */
    } else if (!strcmp(method, "signalStatus")) {
        on_signal(&T.info, j);
        log_once(1, method, j);
        T.changed = 1;
    } else if (!strcmp(method, "descrambleInfo")) {
        on_descramble(&T.info, j);
        log_once(2, method, j);
        T.changed = 1;
    } else if (!strcmp(method, "subscriptionStatus")) {
        if (js(j, "status")[0] || js(j, "subscriptionError")[0]) {
            mt_log("[tvhstat] subscriptionStatus %s %s", js(j, "status"), js(j, "subscriptionError"));
            on_error(&T.info, j);
            T.changed = 1;
        }
    } else if (!strcmp(method, "subscriptionStop")) {
        on_error(&T.info, j);
        T.changed = 1;
    }
    pthread_mutex_unlock(&T.lock);
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
