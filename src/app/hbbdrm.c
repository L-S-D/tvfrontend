/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "hbbdrm.h"

#include "cJSON.h"
#include "log.h"
#include "luna.h"
#include "relay.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID "com.lsd.tvf"
#define DRM "luna://com.webos.service.drm/"

enum { K_ISLOADED, K_LOAD, K_SEND, K_RIGHTS, K_OTHER };

/* Message to the client, waiting for the client or for the reply */
typedef struct pend {
    struct pend *next;
    char *msg_type, *msg, *system_id;
    hbbdrm_result_fn cb;
    void *user;
} pend;

typedef struct rmsg {
    struct rmsg *next;
    int kind;
    pend *p; /* K_SEND */
    char text[];
} rmsg;

static void (*g_wake)(void);
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static rmsg *g_head, *g_tail;

static struct {
    char client[48];
    int loading;
    pend *waiting; /* messages until the client is there */
    luna_sub *rights;
    void (*on_rights)(int, const char *);
} S;

typedef struct {
    int kind;
    pend *p;
} rctx;

static void on_luna(const char *reply, void *user) {
    rctx *c = user;
    size_t n = reply ? strlen(reply) : 0;
    rmsg *m = malloc(sizeof(rmsg) + n + 1);
    if (m) {
        m->next = NULL;
        m->kind = c->kind;
        m->p = c->p;
        memcpy(m->text, reply ? reply : "", n + 1);
        pthread_mutex_lock(&g_lock);
        if (g_tail) g_tail->next = m;
        else g_head = m;
        g_tail = m;
        pthread_mutex_unlock(&g_lock);
        if (g_wake) g_wake();
    }
    if (c->kind != K_RIGHTS) free(c);
}

static int call(const char *method, const char *payload, int kind, pend *p) {
    char uri[96];
    snprintf(uri, sizeof(uri), DRM "%s", method);
    rctx *c = malloc(sizeof(*c));
    if (!c) return -1;
    c->kind = kind;
    c->p = p;
    mt_log("[drm] %s %.300s", method, payload);
    if (kind == K_RIGHTS) {
        S.rights = luna_subscribe(uri, payload, on_luna, c);
        if (!S.rights) {
            free(c);
            return -1;
        }
        return 0;
    }
    if (luna_call(uri, payload, on_luna, c) != 0) {
        free(c);
        mt_log("[drm] %s failed", method);
        return -1;
    }
    return 0;
}

static void free_pend(pend *p) {
    if (!p) return;
    free(p->msg_type);
    free(p->msg);
    free(p->system_id);
    free(p);
}

static void do_send(pend *p) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "clientId", S.client);
    cJSON_AddStringToObject(j, "msgType", p->msg_type);
    cJSON_AddStringToObject(j, "msg", p->msg);
    cJSON_AddStringToObject(j, "drmSystemId", p->system_id);
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!s || call("sendDrmMessage", s, K_SEND, p) != 0) {
        if (p->cb) p->cb(p->user, 1, "");
        free_pend(p);
    }
    free(s);
}

void hbbdrm_init(void (*wake)(void)) { g_wake = wake; }

const char *hbbdrm_client(void) { return S.client[0] ? S.client : NULL; }

void hbbdrm_on_rights(void (*fn)(int, const char *)) { S.on_rights = fn; }

int hbbdrm_load(void) {
    if (S.client[0] || S.loading) return 0;
    S.loading = 1;
    /* reuse a leftover (crash): one client per app */
    return call("isLoaded", "{\"appId\":\"" APP_ID "\"}", K_ISLOADED, NULL);
}

/* Licence requests via the relay: LA_URL in PlayReady initiators
 * (override and WRMHEADER) points to it. It forwards the POST according to the
 * egress rules (channel rule as for pages and videos - the TV's
 * DRM client knows no proxy) and writes one line per licence
 * to the log, with mode 2 the full request and response. Default: on. */
static int g_proxy = 1;

void hbbdrm_proxy(int mode) {
    g_proxy = mode > 0;
    relay_license_dump(mode >= 2);
    mt_log("[drm] licenses %s", mode >= 2 ? "via relay, full capture" : mode ? "via relay" : "direct");
}

/* XML text -> plain text (&amp; &lt; &gt; &quot; &apos;) */
static void xml_text(const char *s, size_t n, char *out, size_t on) {
    static const char *ent[][2] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}};
    size_t o = 0;
    for (size_t i = 0; i < n && o + 1 < on;) {
        size_t k = 0;
        for (; k < 5; k++) {
            size_t el = strlen(ent[k][0]);
            if (i + el <= n && !strncmp(s + i, ent[k][0], el)) {
                out[o++] = ent[k][1][0];
                i += el;
                break;
            }
        }
        if (k == 5) out[o++] = s[i++];
    }
    out[o] = 0;
}

static char *proxy_laurl(const char *msg) {
    const char *a = strstr(msg, "<LA_URL>"), *b = a ? strstr(a, "</LA_URL>") : NULL;
    if (!a || !b || b == a + 8) return NULL; /* no or empty LA_URL (reset) */
    a += 8;
    char url[3000], rel[3600], esc[4800];
    xml_text(a, (size_t) (b - a), url, sizeof(url));
    if (strncmp(url, "http://", 7) && strncmp(url, "https://", 8)) return NULL;
    relay_license_url(url, rel, sizeof(rel));
    if (!rel[0]) return NULL;
    size_t o = 0;
    for (const char *p = rel; *p && o + 6 < sizeof(esc); p++) {
        if (*p == '&') {
            memcpy(esc + o, "&amp;", 5);
            o += 5;
        } else {
            esc[o++] = *p;
        }
    }
    esc[o] = 0;
    size_t pre = (size_t) (a - msg), post = strlen(b);
    char *m = malloc(pre + o + post + 1);
    if (!m) return NULL;
    memcpy(m, msg, pre);
    memcpy(m + pre, esc, o);
    memcpy(m + pre + o, b, post + 1);
    mt_log("[drm] LA_URL %s -> relay", url);
    return m;
}

void hbbdrm_send(const char *msg_type, const char *msg, const char *system_id, hbbdrm_result_fn cb, void *user) {
    pend *p = calloc(1, sizeof(*p));
    if (!p) {
        if (cb) cb(user, 1, "");
        return;
    }
    p->msg_type = strdup(msg_type ? msg_type : HBBDRM_INITIATOR);
    p->msg = g_proxy && msg && !strcmp(p->msg_type, HBBDRM_INITIATOR) ? proxy_laurl(msg) : NULL;
    if (!p->msg) p->msg = strdup(msg ? msg : "");
    p->system_id = strdup(system_id ? system_id : HBBDRM_PLAYREADY);
    p->cb = cb;
    p->user = user;
    if (S.client[0]) {
        do_send(p);
        return;
    }
    p->next = S.waiting;
    S.waiting = p;
    hbbdrm_load();
}

void hbbdrm_reset(void) {
    if (!S.client[0]) return;
    hbbdrm_send(HBBDRM_INITIATOR,
                "<PlayReadyInitiator xmlns=\"http://schemas.microsoft.com/DRM/2007/03/protocols/\">"
                "<LicenseServerUriOverride><LA_URL></LA_URL></LicenseServerUriOverride>"
                "<SetCustomData><CustomData></CustomData></SetCustomData></PlayReadyInitiator>",
                HBBDRM_PLAYREADY, NULL, NULL);
}

void hbbdrm_unload(void) {
    if (S.rights) luna_unsubscribe(S.rights);
    S.rights = NULL;
    if (S.client[0]) {
        char p[96];
        snprintf(p, sizeof(p), "{\"clientId\":\"%s\"}", S.client);
        call("unload", p, K_OTHER, NULL);
    }
    S.client[0] = 0;
    S.loading = 0;
}

static void got_client(const char *id) {
    snprintf(S.client, sizeof(S.client), "%s", id);
    S.loading = 0;
    mt_log("[drm] PlayReady client %s", S.client);
    char p[128];
    snprintf(p, sizeof(p), "{\"clientId\":\"%s\",\"subscribe\":true}", S.client);
    call("getRightsError", p, K_RIGHTS, NULL);
    /* send waiting messages in order */
    pend *rev = NULL;
    while (S.waiting) {
        pend *n = S.waiting->next;
        S.waiting->next = rev;
        rev = S.waiting;
        S.waiting = n;
    }
    while (rev) {
        pend *n = rev->next;
        rev->next = NULL;
        do_send(rev);
        rev = n;
    }
}

static void fail_waiting(void) {
    S.loading = 0;
    while (S.waiting) {
        pend *n = S.waiting->next;
        if (S.waiting->cb) S.waiting->cb(S.waiting->user, 1, "");
        free_pend(S.waiting);
        S.waiting = n;
    }
}

static void on_msg(rmsg *m) {
    cJSON *j = cJSON_Parse(m->text);
    const cJSON *rv = cJSON_GetObjectItemCaseSensitive(j, "returnValue");
    const cJSON *cid = cJSON_GetObjectItemCaseSensitive(j, "clientId");
    mt_log("[drm] reply %.300s", m->text);
    switch (m->kind) {
    case K_ISLOADED: {
        const cJSON *ls = cJSON_GetObjectItemCaseSensitive(j, "loadStatus");
        if (cJSON_IsTrue(ls) && cJSON_IsString(cid) && cid->valuestring[0]) {
            got_client(cid->valuestring);
        } else if (call("load", "{\"drmType\":\"playready\",\"appId\":\"" APP_ID "\"}", K_LOAD, NULL) != 0) {
            fail_waiting();
        }
        break;
    }
    case K_LOAD:
        if (cJSON_IsTrue(rv) && cJSON_IsString(cid)) got_client(cid->valuestring);
        else fail_waiting();
        break;
    case K_SEND: {
        pend *p = m->p;
        int code = 1;
        const char *rmsgs = "";
        if (cJSON_IsTrue(rv)) {
            const cJSON *rc = cJSON_GetObjectItemCaseSensitive(j, "resultCode");
            const cJSON *rm = cJSON_GetObjectItemCaseSensitive(j, "resultMsg");
            code = cJSON_IsNumber(rc) ? (int) rc->valuedouble : 0;
            if (cJSON_IsString(rm)) rmsgs = rm->valuestring;
        }
        if (p && p->cb) p->cb(p->user, code, rmsgs);
        free_pend(p);
        break;
    }
    case K_RIGHTS: {
        const cJSON *es = cJSON_GetObjectItemCaseSensitive(j, "errorState");
        const cJSON *ci = cJSON_GetObjectItemCaseSensitive(j, "contentId");
        if (es && S.on_rights) {
            int st = cJSON_IsString(es) ? atoi(es->valuestring) : cJSON_IsNumber(es) ? (int) es->valuedouble : 0;
            S.on_rights(st, cJSON_IsString(ci) ? ci->valuestring : "");
        }
        break;
    }
    default: break;
    }
    cJSON_Delete(j);
}

void hbbdrm_poll(void) {
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
}
