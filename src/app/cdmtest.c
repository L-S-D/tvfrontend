/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Test of the TV's CDMs without WebKit (debug command CDMTEST):
 *
 *   CDMTEST <key system> <licence server> <init data base64 (PSSH)>
 *
 * Create a session, send the licence request to the server (Widevine: body as
 * is; PlayReady: SOAP from the message), install the response, log the keys,
 * then push an artificial encrypted sample through Decrypt (shows size and
 * tail of the data for the secure path). Everything in a thread of its own,
 * result in the log ([cdmtest]).
 */
#define _GNU_SOURCE
#include "cdmtest.h"

#include "cdm.h"
#include "http.h"
#include "log.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int busy;
    uint8_t *msg;
    uint32_t msglen;
    int msgtype;
    uint8_t kid[16];
    uint32_t kidlen;
    int usable;
} T = {PTHREAD_MUTEX_INITIALIZER, PTHREAD_COND_INITIALIZER, 0, NULL, 0, 0, {0}, 0, 0};

static int b64val(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

static uint8_t *b64dec(const char *s, uint32_t *n) {
    size_t l = strlen(s);
    uint8_t *o = malloc(l * 3 / 4 + 3);
    uint32_t acc = 0, bits = 0, k = 0;
    for (size_t i = 0; o && i < l; i++) {
        int v = b64val((unsigned char) s[i]);
        if (v < 0) continue;
        acc = acc << 6 | (uint32_t) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            o[k++] = (uint8_t) (acc >> bits);
        }
    }
    *n = k;
    return o;
}

static void hex(const uint8_t *d, uint32_t n, char *out, size_t on) {
    size_t o = 0;
    for (uint32_t i = 0; i < n && o + 3 < on; i++) o += (size_t) snprintf(out + o, on - o, "%02x", d[i]);
    out[o] = 0;
}

static void ev_message(void *user, const char *sid, int type, const uint8_t *msg, uint32_t len) {
    (void) user;
    pthread_mutex_lock(&T.lock);
    free(T.msg);
    T.msg = malloc(len ? len : 1);
    if (T.msg) memcpy(T.msg, msg, len);
    T.msglen = T.msg ? len : 0;
    T.msgtype = type;
    pthread_cond_broadcast(&T.cond);
    pthread_mutex_unlock(&T.lock);
    mt_log("[cdmtest] %s: message type %d, %u bytes", sid, type, (unsigned) len);
}

static void ev_keys(void *user, const char *sid, const cdm_key *keys, int count) {
    (void) user;
    (void) sid;
    pthread_mutex_lock(&T.lock);
    for (int i = 0; i < count; i++)
        if (keys[i].status == CDM_KEY_USABLE && keys[i].len <= 16) {
            memcpy(T.kid, keys[i].id, keys[i].len);
            T.kidlen = keys[i].len;
            T.usable = 1;
        }
    pthread_cond_broadcast(&T.cond);
    pthread_mutex_unlock(&T.lock);
}

static void ev_closed(void *user, const char *sid) {
    (void) user;
    mt_log("[cdmtest] %s closed", sid);
}

/* PlayReady: message = PlayReadyKeyMessage (UTF-16) with challenge (base64)
 * and headers, or already the SOAP request */
static uint8_t *playready_body(const uint8_t *m, uint32_t n, uint32_t *bn, char *hdr, size_t hn) {
    char *a = malloc(n + 1);
    uint32_t k = 0;
    int utf16 = n > 1 && m[1] == 0;
    for (uint32_t i = 0; a && i < n; i += utf16 ? 2 : 1) a[k++] = (char) m[i];
    if (!a) return NULL;
    a[k] = 0;
    snprintf(hdr, hn, "Content-Type: text/xml; charset=utf-8\r\n"
                      "SOAPAction: \"http://schemas.microsoft.com/DRM/2007/03/protocols/AcquireLicense\"\r\n");
    char *c = strstr(a, "<Challenge");
    if (c && (c = strchr(c, '>'))) {
        char *e = strstr(++c, "</Challenge>");
        if (e) *e = 0;
        uint8_t *b = b64dec(c, bn);
        free(a);
        return b;
    }
    *bn = k;
    return (uint8_t *) a;
}

typedef struct {
    char ks[64], url[1024];
    char *init;
} job;

static void *run(void *arg) {
    job *j = arg;
    char err[256] = "", sid[256] = "";
    uint32_t initlen = 0;
    uint8_t *init = b64dec(j->init, &initlen);
    static const cdm_events ev = {ev_message, ev_keys, ev_closed};
    pthread_mutex_lock(&T.lock);
    free(T.msg);
    T.msg = NULL;
    T.msglen = 0;
    T.usable = 0;
    pthread_mutex_unlock(&T.lock);
    int64_t t0 = mt_now_ns();
    tvf_cdm *c = cdm_open(j->ks, &ev, NULL, err, sizeof(err));
    if (!c) {
        mt_log("[cdmtest] %s: no CDM: %s", j->ks, err);
        goto done;
    }
    mt_log("[cdmtest] %s open after %.0f ms", j->ks, (mt_now_ns() - t0) / 1e6);
    if (cdm_create_session(c, 0, "cenc", init, initlen, sid, sizeof(sid), err, sizeof(err))) {
        mt_log("[cdmtest] session rejected: %s", err);
        goto close;
    }
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 10;
    pthread_mutex_lock(&T.lock);
    while (!T.msg) {
        if (pthread_cond_timedwait(&T.cond, &T.lock, &until)) break;
    }
    uint8_t *msg = T.msg;
    uint32_t msglen = T.msglen;
    T.msg = NULL;
    pthread_mutex_unlock(&T.lock);
    if (!msg) {
        mt_log("[cdmtest] no license request");
        goto close;
    }
    char h[96];
    hex(msg, msglen < 24 ? msglen : 24, h, sizeof(h));
    mt_log("[cdmtest] request %u bytes: %s...", (unsigned) msglen, h);
    char hdr[512] = "Content-Type: application/octet-stream\r\n";
    uint8_t *body = msg;
    uint32_t bodylen = msglen;
    if (strstr(j->ks, "playready")) body = playready_body(msg, msglen, &bodylen, hdr, sizeof(hdr));
    http_conn hc;
    int64_t t1 = mt_now_ns();
    if (!body || http_request("POST", j->url, hdr, body, bodylen, NULL, &hc) != 0) {
        mt_log("[cdmtest] POST %s failed", j->url);
        if (body != msg) free(body);
        free(msg);
        goto close;
    }
    if (body != msg) free(body);
    free(msg);
    size_t cap = 65536, got = 0;
    uint8_t *resp = malloc(cap);
    ssize_t r;
    while (resp && (r = http_read(&hc, resp + got, cap - got)) > 0) {
        got += (size_t) r;
        if (got == cap) {
            uint8_t *nr = realloc(resp, cap *= 2);
            if (!nr) break;
            resp = nr;
        }
    }
    int status = hc.status;
    http_close(&hc);
    mt_log("[cdmtest] license server: HTTP %d, %zu bytes after %.0f ms", status, got, (mt_now_ns() - t1) / 1e6);
    if (status != 200 || !resp) {
        if (resp && got) mt_log("[cdmtest] response: %.*s", (int) (got < 300 ? got : 300), (char *) resp);
        free(resp);
        goto close;
    }
    int rc = cdm_update_session(c, sid, resp, (uint32_t) got, err, sizeof(err));
    free(resp);
    mt_log("[cdmtest] license installed: %s", rc ? err : "ok");
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 5;
    pthread_mutex_lock(&T.lock);
    while (!T.usable)
        if (pthread_cond_timedwait(&T.cond, &T.lock, &until)) break;
    int usable = T.usable;
    uint8_t kid[16];
    uint32_t kidlen = T.kidlen;
    memcpy(kid, T.kid, sizeof(kid));
    pthread_mutex_unlock(&T.lock);
    if (!usable) {
        mt_log("[cdmtest] no usable key");
        goto close;
    }
    /* artificial sample: 4 bytes clear (start code), rest encrypted */
    uint8_t sample[2052];
    for (size_t i = 0; i < sizeof(sample); i++) sample[i] = (uint8_t) (i * 7);
    sample[0] = sample[1] = sample[2] = 0, sample[3] = 1, sample[4] = 0x65;
    uint32_t subs[2] = {5, 2047};
    uint8_t iv[16] = {1, 2, 3, 4, 5, 6, 7, 8};
    /* cenc with subsample; then a clear sample described three different ways:
     * like inputstream.adaptive (cenc without IV), as unencrypted, as cenc
     * with only a clear subsample */
    uint8_t zero[16] = {0};
    uint32_t clear[2] = {sizeof(sample), 0};
    cdm_sample tests[4] = {
        {1, kid, kidlen, iv, 16, subs, 1, 0, 0, 0},
        {1, kid, kidlen, NULL, 16, NULL, 0, 0, 0, 0},
        {0, kid, kidlen, NULL, 0, NULL, 0, 0, 0, 0},
        {1, kid, kidlen, zero, 16, clear, 1, 0, 0, 0},
    };
    static const char *names[4] = {"cenc", "clear/ISA", "clear/unencrypted", "clear/cenc 0 encr."};
    for (int k = 0; k < 4; k++) {
        uint8_t *out = NULL;
        uint32_t outn = 0;
        int64_t t2 = mt_now_ns();
        int d = cdm_decrypt(c, 1, &tests[k], sample, sizeof(sample), &out, &outn);
        char head[200], tail[160];
        hex(out, out && outn > 80 ? 80 : outn, head, sizeof(head));
        hex(out && outn > 64 ? out + outn - 64 : out, out && outn > 64 ? 64 : outn, tail, sizeof(tail));
        mt_log("[cdmtest] Decrypt(%s) -> %d, %u bytes from %zu in %.2f ms; head %s; tail %s", names[k], d,
               (unsigned) outn, sizeof(sample), (mt_now_ns() - t2) / 1e6, head, tail);
        free(out);
    }
close:
    if (sid[0]) cdm_close_session(c, sid, err, sizeof(err));
    if (c) cdm_close(c);
done:
    mt_log("[cdmtest] done");
    free(init);
    free(j->init);
    free(j);
    pthread_mutex_lock(&T.lock);
    T.busy = 0;
    pthread_mutex_unlock(&T.lock);
    return NULL;
}

void cdmtest_start(const char *args) {
    job *j = calloc(1, sizeof(*j));
    char init[8192];
    if (!j || sscanf(args, "%63s %1023s %8191s", j->ks, j->url, init) != 3) {
        mt_log("[cdmtest] usage: CDMTEST <key system> <license server> <PSSH base64>");
        free(j);
        return;
    }
    pthread_mutex_lock(&T.lock);
    int busy = T.busy;
    T.busy = 1;
    pthread_mutex_unlock(&T.lock);
    if (busy) {
        mt_log("[cdmtest] already running");
        free(j);
        return;
    }
    j->init = strdup(init);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &at, run, j) != 0) {
        free(j->init);
        free(j);
        pthread_mutex_lock(&T.lock);
        T.busy = 0;
        pthread_mutex_unlock(&T.lock);
    }
    pthread_attr_destroy(&at);
}
