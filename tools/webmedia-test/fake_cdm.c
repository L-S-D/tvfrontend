/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Emulated CDM for the test bench (instead of src/app/cdm.cpp, which loads the
 * TV's modules): same interface (cdm.h), flow like a
 * Chromium CDM, but with readable data:
 *   create session -> message "CHALLENGE:<init data hex>" (still BEFORE
 *     returning, as some modules do - tests the queue in webdrm.c)
 *   response "LICENSE:<KID hex 32>" -> key usable (event before returning)
 *   response "NOKEYS" -> accepted, but without a key event
 *   load session "<id>" -> key 11..11
 *   Decrypt -> data + "FAKESVP!" + number of subsamples (u32), kNoKey without a key
 */
#include "cdm.h"
#include "log.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAXS 16

struct tvf_cdm {
    int refs, closed;
    char ks[64];
    cdm_events ev;
    void *user;
    char sids[MAXS][64];
    uint8_t kid[MAXS][16];
    int haskey[MAXS];
    struct tvf_cdm *next;
};

static pthread_mutex_t L = PTHREAD_MUTEX_INITIALIZER;
static struct tvf_cdm *open_list;
static int next_sid = 1;

int cdm_supported(const char *ks) {
    return ks && (!strcmp(ks, "com.widevine.alpha") || !strcmp(ks, "com.microsoft.playready"));
}
const char *cdm_drm_type(const char *ks) {
    if (ks && !strcmp(ks, "com.widevine.alpha")) return "WIDEVINE_MODULAR";
    if (ks && !strncmp(ks, "com.microsoft.playready", 23)) return "PLAYREADY";
    return NULL;
}
const char *cdm_key_system_for_uuid(const char *uuid) {
    if (uuid && !strcasecmp(uuid, "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed")) return "com.widevine.alpha";
    if (uuid && !strcasecmp(uuid, "9a04f079-9840-4286-ab92-e65be0885f95")) return "com.microsoft.playready";
    return NULL;
}

tvf_cdm *cdm_open(const char *ks, const cdm_events *ev, void *user, char *err, size_t errn) {
    if (!cdm_supported(ks)) {
        snprintf(err, errn, "unknown");
        return NULL;
    }
    tvf_cdm *c = calloc(1, sizeof(*c));
    c->refs = 1;
    snprintf(c->ks, sizeof(c->ks), "%s", ks);
    c->ev = *ev;
    c->user = user;
    pthread_mutex_lock(&L);
    c->next = open_list;
    open_list = c;
    pthread_mutex_unlock(&L);
    mt_log("[fakecdm] %s open", ks);
    return c;
}

void cdm_unref(tvf_cdm *c) {
    pthread_mutex_lock(&L);
    int last = --c->refs == 0;
    pthread_mutex_unlock(&L);
    if (last) {
        mt_log("[fakecdm] %s destroyed", c->ks);
        free(c);
    }
}

void cdm_close(tvf_cdm *c) {
    pthread_mutex_lock(&L);
    for (tvf_cdm **p = &open_list; *p; p = &(*p)->next)
        if (*p == c) {
            *p = c->next;
            break;
        }
    c->closed = 1;
    pthread_mutex_unlock(&L);
    mt_log("[fakecdm] %s closed", c->ks);
    cdm_unref(c);
}

const char *cdm_key_system(const tvf_cdm *c) { return c->ks; }

int cdm_set_certificate(tvf_cdm *c, const uint8_t *d, uint32_t n, char *err, size_t errn) {
    (void) c, (void) d, (void) err, (void) errn;
    mt_log("[fakecdm] certificate %u bytes", (unsigned) n);
    return 0;
}

static int slot(tvf_cdm *c, const char *sid) {
    for (int i = 0; i < MAXS; i++)
        if (!strcmp(c->sids[i], sid)) return i;
    return -1;
}

int cdm_create_session(tvf_cdm *c, int persistent, const char *init_type, const uint8_t *d, uint32_t n, char *sid,
                       size_t sidn, char *err, size_t errn) {
    (void) persistent;
    pthread_mutex_lock(&L);
    int i = slot(c, "");
    if (i < 0) {
        pthread_mutex_unlock(&L);
        snprintf(err, errn, "full");
        return -1;
    }
    snprintf(c->sids[i], sizeof(c->sids[i]), "fake-%d", next_sid++);
    snprintf(sid, sidn, "%s", c->sids[i]);
    pthread_mutex_unlock(&L);
    char msg[600] = "CHALLENGE:";
    size_t o = strlen(msg);
    for (uint32_t k = 0; k < n && o + 3 < sizeof(msg); k++) o += (size_t) snprintf(msg + o, sizeof(msg) - o, "%02x", d[k]);
    mt_log("[fakecdm] session %s (%s, %u bytes)", sid, init_type, (unsigned) n);
    /* message before returning (webdrm.c does not know the session yet) */
    c->ev.message(c->user, sid, 0, (const uint8_t *) msg, (uint32_t) strlen(msg));
    return 0;
}

static void keys_event(tvf_cdm *c, int i) {
    cdm_key k = {c->kid[i], 16, CDM_KEY_USABLE};
    c->ev.keys(c->user, c->sids[i], &k, 1);
}

int cdm_load_session(tvf_cdm *c, const char *sid, char *err, size_t errn) {
    pthread_mutex_lock(&L);
    int i = slot(c, "");
    if (i >= 0) {
        snprintf(c->sids[i], sizeof(c->sids[i]), "%s", sid);
        memset(c->kid[i], 0x11, 16);
        c->haskey[i] = 1;
    }
    pthread_mutex_unlock(&L);
    if (i < 0) {
        snprintf(err, errn, "full");
        return -1;
    }
    keys_event(c, i);
    return 0;
}

int cdm_update_session(tvf_cdm *c, const char *sid, const uint8_t *d, uint32_t n, char *err, size_t errn) {
    pthread_mutex_lock(&L);
    int i = slot(c, sid);
    pthread_mutex_unlock(&L);
    if (i < 0) {
        snprintf(err, errn, "unknown session");
        return -1;
    }
    if (n == 6 && !memcmp(d, "NOKEYS", 6)) {
        mt_log("[fakecdm] %s: response without keys", sid);
        return 0;
    }
    if (n < 8 + 32 || (n - 8) % 32 || memcmp(d, "LICENSE:", 8)) {
        snprintf(err, errn, "broken license (%u bytes)", (unsigned) n);
        return -1;
    }
    /* one or more KIDs (32 hex each); the first belongs to the session, the
     * others get their own slots under the same ID */
    int nk = (int) (n - 8) / 32, first = i;
    for (int q = 0; q < nk; q++) {
        int slotq = q ? -1 : i;
        if (q) {
            pthread_mutex_lock(&L);
            slotq = slot(c, "");
            if (slotq >= 0) snprintf(c->sids[slotq], sizeof(c->sids[slotq]), "%s#%d", sid, q);
            pthread_mutex_unlock(&L);
            if (slotq < 0) break;
        }
        for (int k = 0; k < 16; k++) {
            unsigned v;
            sscanf((const char *) d + 8 + 32 * q + 2 * k, "%2x", &v);
            c->kid[slotq][k] = (uint8_t) v;
        }
        c->haskey[slotq] = 1;
    }
    mt_log("[fakecdm] %s: license ok (%d keys)", sid, nk);
    cdm_key k[MAXS];
    int kn = 0;
    for (int q = 0; q < MAXS && kn < nk; q++)
        if (c->haskey[q] && (q == first || !strncmp(c->sids[q], sid, strlen(sid)))) {
            k[kn].id = c->kid[q];
            k[kn].len = 16;
            k[kn++].status = CDM_KEY_USABLE;
        }
    c->ev.keys(c->user, sid, k, kn);
    return 0;
}

int cdm_close_session(tvf_cdm *c, const char *sid, char *err, size_t errn) {
    (void) err, (void) errn;
    pthread_mutex_lock(&L);
    int i = slot(c, sid);
    if (i >= 0) c->sids[i][0] = 0, c->haskey[i] = 0;
    pthread_mutex_unlock(&L);
    mt_log("[fakecdm] %s closed", sid);
    return 0;
}

int cdm_remove_session(tvf_cdm *c, const char *sid, char *err, size_t errn) {
    return cdm_close_session(c, sid, err, errn);
}

int cdm_decrypt(tvf_cdm *c, int video, const cdm_sample *s, const uint8_t *data, uint32_t n, uint8_t **out,
                uint32_t *outn) {
    (void) video;
    int ok = 0;
    pthread_mutex_lock(&L);
    for (int i = 0; i < MAXS; i++)
        if (c->haskey[i] && (!s->kid || !s->kid_len || (s->kid_len == 16 && !memcmp(c->kid[i], s->kid, 16)))) ok = 1;
    pthread_mutex_unlock(&L);
    if (!ok) return 2;
    /* check the subsamples: they must cover exactly the data */
    uint64_t total = 0;
    for (uint32_t i = 0; i < s->nsubs; i++) total += (uint64_t) s->subs[2 * i] + s->subs[2 * i + 1];
    if (s->iv && s->nsubs && total != n) {
        mt_log("[fakecdm] subsamples %llu B, data %u B", (unsigned long long) total, (unsigned) n);
        return 4;
    }
    *out = malloc(n + 12);
    memcpy(*out, data, n);
    memcpy(*out + n, "FAKESVP!", 8);
    memcpy(*out + n + 8, &s->nsubs, 4);
    *outn = n + 12;
    return 0;
}

tvf_cdm *cdm_find_session(const char *sid) {
    pthread_mutex_lock(&L);
    tvf_cdm *r = NULL;
    for (tvf_cdm *c = open_list; c && !r; c = c->next)
        if (slot(c, sid) >= 0) r = c;
    if (r) r->refs++;
    pthread_mutex_unlock(&L);
    return r;
}

tvf_cdm *cdm_find_system(const char *ks) {
    pthread_mutex_lock(&L);
    tvf_cdm *r = NULL;
    for (tvf_cdm *c = open_list; c && !r; c = c->next)
        if (!strcmp(c->ks, ks)) r = c;
    if (r) r->refs++;
    pthread_mutex_unlock(&L);
    return r;
}
