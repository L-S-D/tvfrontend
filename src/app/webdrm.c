/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * EME connections, see webdrm.h.
 *
 * Per connection (one WebProcess) the client's systems (one CDM instance per
 * MediaKeys, created only when first needed) and sessions. Requests run one
 * after another in the reader thread and wait for the CDM; events of the CDM
 * (licence request, keys, close) come from its threads and go to the
 * connection under D.lock. An event for a session whose CREATE reply has not
 * been sent yet waits until afterwards (order as in EME: first the session,
 * then its message).
 */
#define _GNU_SOURCE
#include "webdrm.h"

#include "cdm.h"
#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

enum { D_OPEN = 20, D_CLOSE, D_CERT, D_CREATE, D_LOAD, D_UPDATE, D_REMOVE, D_SCLOSE, D_RELEASE, D_QUERY };
enum { R_REPLY = 120, R_MESSAGE, R_KEYS, R_ERROR, R_CLOSED };

typedef struct {
    uint8_t *d;
    size_t n, cap;
} buf;

static void b_put(buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        size_t c = b->cap ? b->cap : 256;
        while (c < b->n + n) c *= 2;
        uint8_t *d = realloc(b->d, c);
        if (!d) return;
        b->d = d;
        b->cap = c;
    }
    if (n) memcpy(b->d + b->n, p, n);
    b->n += n;
}
static void b_u32(buf *b, uint32_t v) { b_put(b, &v, 4); }
static void b_bytes(buf *b, const void *p, uint32_t n) {
    b_u32(b, n);
    b_put(b, p, n);
}
static void b_str(buf *b, const char *s) { b_bytes(b, s, (uint32_t) strlen(s)); }

typedef struct {
    const uint8_t *p, *end;
    int bad;
} rd;
static uint32_t r_u32(rd *r) {
    uint32_t v = 0;
    if (r->end - r->p < 4) {
        r->bad = 1;
        return 0;
    }
    memcpy(&v, r->p, 4);
    r->p += 4;
    return v;
}
static const uint8_t *r_bytes(rd *r, uint32_t *n) {
    *n = r_u32(r);
    if (r->bad || (uint32_t) (r->end - r->p) < *n) {
        r->bad = 1;
        *n = 0;
        return NULL;
    }
    const uint8_t *d = r->p;
    r->p += *n;
    return d;
}
static void r_str(rd *r, char *out, size_t on) {
    uint32_t n;
    const uint8_t *d = r_bytes(r, &n);
    snprintf(out, on, "%.*s", (int) n, d ? (const char *) d : "");
}

struct dconn;

typedef struct pendev {
    struct pendev *next;
    char sid[256];
    uint32_t type;
    buf b;
} pendev;

typedef struct dsys {
    struct dsys *next;
    uint32_t gid, id;
    struct dconn *conn;
    char ks[64];
    tvf_cdm *cdm;
    pendev *pend; /* events for sessions not yet reported */
} dsys;

typedef struct dsess {
    struct dsess *next;
    uint32_t id, gid; /* client's ID, system */
    char sid[256];
    int open;
} dsess;

typedef struct dconn {
    int fd;
    pthread_mutex_t wlock;
    dsess *sess;
} dconn;

static struct {
    pthread_mutex_t lock; /* systems, sessions, queues */
    uint32_t next_gid;
    dsys *all;
} D = {PTHREAD_MUTEX_INITIALIZER, 0, NULL};

static void send_msg(dconn *c, uint32_t type, const buf *b) {
    uint32_t h[2] = {type, (uint32_t) b->n};
    struct iovec iov[2] = {{h, sizeof(h)}, {b->d, b->n}};
    struct msghdr m = {0};
    m.msg_iov = iov;
    m.msg_iovlen = b->n ? 2 : 1;
    size_t total = sizeof(h) + b->n, done = 0;
    pthread_mutex_lock(&c->wlock);
    while (done < total) {
        ssize_t r = sendmsg(c->fd, &m, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        done += (size_t) r;
        while (m.msg_iovlen && (size_t) r >= m.msg_iov->iov_len) {
            r -= (ssize_t) m.msg_iov->iov_len;
            m.msg_iov++;
            m.msg_iovlen--;
        }
        if (m.msg_iovlen && r) {
            m.msg_iov->iov_base = (char *) m.msg_iov->iov_base + r;
            m.msg_iov->iov_len -= (size_t) r;
        }
    }
    pthread_mutex_unlock(&c->wlock);
}

static void reply(dconn *c, uint32_t req, uint32_t status, const char *text) {
    buf b = {0};
    b_u32(&b, req);
    b_u32(&b, status);
    b_str(&b, text ? text : "");
    send_msg(c, R_REPLY, &b);
    free(b.d);
}

static dsys *sys_by_gid(uint32_t gid) {
    for (dsys *y = D.all; y; y = y->next)
        if (y->gid == gid) return y;
    return NULL;
}
static dsys *sys_by_id(dconn *c, uint32_t id) {
    for (dsys *y = D.all; y; y = y->next)
        if (y->conn == c && y->id == id) return y;
    return NULL;
}
static dsess *sess_by_sid(dconn *c, uint32_t gid, const char *sid) {
    for (dsess *s = c->sess; s; s = s->next)
        if (s->gid == gid && !strcmp(s->sid, sid)) return s;
    return NULL;
}
static dsess *sess_by_id(dconn *c, uint32_t id) {
    for (dsess *s = c->sess; s; s = s->next)
        if (s->id == id) return s;
    return NULL;
}

/* CDM event to the session (b starts with room for its ID) */
static void deliver(uint32_t gid, const char *sid, uint32_t type, buf *b) {
    pthread_mutex_lock(&D.lock);
    dsys *y = sys_by_gid(gid);
    if (y) {
        dsess *s = sess_by_sid(y->conn, gid, sid);
        if (s) {
            memcpy(b->d, &s->id, 4);
            send_msg(y->conn, type, b);
        } else {
            pendev *e = calloc(1, sizeof(*e));
            if (e) {
                snprintf(e->sid, sizeof(e->sid), "%s", sid);
                e->type = type;
                e->b = *b;
                b->d = NULL;
                pendev **pp = &y->pend;
                while (*pp) pp = &(*pp)->next;
                *pp = e;
            }
        }
    }
    pthread_mutex_unlock(&D.lock);
    free(b->d);
}

/* session just reported: send its waiting events after it (under D.lock) */
static void flush_pending(dsys *y, dsess *s) {
    for (pendev **pp = &y->pend; *pp;) {
        pendev *e = *pp;
        if (strcmp(e->sid, s->sid)) {
            pp = &e->next;
            continue;
        }
        *pp = e->next;
        memcpy(e->b.d, &s->id, 4);
        send_msg(y->conn, e->type, &e->b);
        free(e->b.d);
        free(e);
    }
}

static void ev_message(void *user, const char *sid, int type, const uint8_t *msg, uint32_t len) {
    buf b = {0};
    b_u32(&b, 0);
    b_u32(&b, (uint32_t) type);
    b_str(&b, "");
    b_bytes(&b, msg, len);
    deliver((uint32_t) (uintptr_t) user, sid, R_MESSAGE, &b);
}

static void ev_keys(void *user, const char *sid, const cdm_key *keys, int count) {
    buf b = {0};
    b_u32(&b, 0);
    b_u32(&b, (uint32_t) count);
    for (int i = 0; i < count; i++) {
        b_bytes(&b, keys[i].id, keys[i].len);
        b_u32(&b, (uint32_t) keys[i].status);
    }
    deliver((uint32_t) (uintptr_t) user, sid, R_KEYS, &b);
}

static void ev_closed(void *user, const char *sid) {
    buf b = {0};
    b_u32(&b, 0);
    deliver((uint32_t) (uintptr_t) user, sid, R_CLOSED, &b);
}

static const cdm_events EVENTS = {ev_message, ev_keys, ev_closed};

/* Client's system, created when first needed (also without OPEN) */
static dsys *get_sys(dconn *c, uint32_t id, const char *ks) {
    pthread_mutex_lock(&D.lock);
    dsys *y = sys_by_id(c, id);
    if (!y && (y = calloc(1, sizeof(*y)))) {
        y->gid = ++D.next_gid;
        y->id = id;
        y->conn = c;
        snprintf(y->ks, sizeof(y->ks), "%s", ks);
        y->next = D.all;
        D.all = y;
    }
    pthread_mutex_unlock(&D.lock);
    return y;
}

static tvf_cdm *ensure_cdm(dsys *y, char *err, size_t errn) {
    if (!y->cdm) y->cdm = cdm_open(y->ks, &EVENTS, (void *) (uintptr_t) y->gid, err, errn);
    return y->cdm;
}

/* MP4 with H.264/HEVC/AAC/AC-3/E-AC-3 - what the sinks and the decoder can handle */
static int mime_ok(const char *mime) {
    if (!*mime) return 1;
    char m[256];
    size_t n = 0;
    for (const char *p = mime; *p && n + 1 < sizeof(m); p++) m[n++] = (char) (*p >= 'A' && *p <= 'Z' ? *p + 32 : *p);
    m[n] = 0;
    if (strncmp(m, "video/mp4", 9) && strncmp(m, "audio/mp4", 9)) return 0;
    const char *c = strstr(m, "codecs=");
    if (!c) return 1;
    static const char *ok[] = {"avc1", "avc3", "hev1", "hvc1", "mp4a", "ac-3", "ec-3"};
    for (c += 7; *c;) {
        while (*c == '"' || *c == '\'' || *c == ' ' || *c == ',') c++;
        if (!*c) break;
        int good = 0;
        for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++)
            if (!strncmp(c, ok[i], 4)) good = 1;
        if (!good) return 0;
        while (*c && *c != ',' && *c != '"' && *c != '\'') c++;
    }
    return 1;
}

static void close_sys(dsys *y) {
    if (y->cdm) cdm_close(y->cdm);
    while (y->pend) {
        pendev *e = y->pend;
        y->pend = e->next;
        free(e->b.d);
        free(e);
    }
    free(y);
}

/* Remove a system's sessions from the connection (under D.lock) */
static void drop_sessions(dconn *c, uint32_t gid) {
    for (dsess **pp = &c->sess; *pp;) {
        dsess *s = *pp;
        if (s->gid == gid) {
            *pp = s->next;
            free(s);
        } else {
            pp = &s->next;
        }
    }
}

static void handle(dconn *c, uint32_t type, rd *r) {
    char ks[64], err[256] = "", sid[256];
    uint32_t req = 0;
    switch (type) {
    case D_OPEN: {
        uint32_t id = r_u32(r);
        r_str(r, ks, sizeof(ks));
        if (!r->bad) get_sys(c, id, ks);
        return;
    }
    case D_CLOSE: {
        uint32_t id = r_u32(r);
        pthread_mutex_lock(&D.lock);
        dsys *y = sys_by_id(c, id);
        if (y) {
            for (dsys **pp = &D.all; *pp; pp = &(*pp)->next)
                if (*pp == y) {
                    *pp = y->next;
                    break;
                }
            drop_sessions(c, y->gid);
        }
        pthread_mutex_unlock(&D.lock);
        if (y) {
            mt_log("[eme] system %u (%s) closed", id, y->ks);
            close_sys(y);
        }
        return;
    }
    case D_QUERY: {
        char mime[256];
        req = r_u32(r);
        r_str(r, ks, sizeof(ks));
        r_str(r, mime, sizeof(mime));
        int ok = !r->bad && cdm_supported(ks) && mime_ok(mime);
        if (!*mime) mt_log("[eme] %s %s", ks, ok ? "available" : "not available");
        reply(c, req, ok ? 0 : 1, "");
        return;
    }
    case D_CERT: {
        req = r_u32(r);
        uint32_t id = r_u32(r), n;
        r_str(r, ks, sizeof(ks));
        const uint8_t *d = r_bytes(r, &n);
        dsys *y = r->bad ? NULL : get_sys(c, id, ks);
        tvf_cdm *m = y ? ensure_cdm(y, err, sizeof(err)) : NULL;
        int rc = m ? cdm_set_certificate(m, d, n, err, sizeof(err)) : -1;
        mt_log("[eme] server certificate (%u bytes): %s", n, rc ? err : "ok");
        reply(c, req, rc ? 1 : 0, rc ? err : "");
        return;
    }
    case D_CREATE: {
        char itype[16];
        req = r_u32(r);
        uint32_t id = r_u32(r);
        r_str(r, ks, sizeof(ks));
        uint32_t sess = r_u32(r), ltype = r_u32(r), n;
        r_str(r, itype, sizeof(itype));
        const uint8_t *d = r_bytes(r, &n);
        dsys *y = r->bad ? NULL : get_sys(c, id, ks);
        tvf_cdm *m = y ? ensure_cdm(y, err, sizeof(err)) : NULL;
        int rc = m ? cdm_create_session(m, ltype == 2, itype, d, n, sid, sizeof(sid), err, sizeof(err)) : -1;
        if (rc) {
            mt_log("[eme] session (%s, %s) rejected: %s", ks, itype, err);
            reply(c, req, 1, err);
            return;
        }
        dsess *s = calloc(1, sizeof(*s));
        pthread_mutex_lock(&D.lock);
        if (s) {
            s->id = sess;
            s->gid = y->gid;
            s->open = 1;
            snprintf(s->sid, sizeof(s->sid), "%s", sid);
            s->next = c->sess;
            c->sess = s;
        }
        reply(c, req, 0, sid);
        if (s) flush_pending(y, s);
        pthread_mutex_unlock(&D.lock);
        return;
    }
    case D_LOAD: {
        req = r_u32(r);
        uint32_t id = r_u32(r);
        r_str(r, ks, sizeof(ks));
        uint32_t sess = r_u32(r);
        r_str(r, sid, sizeof(sid));
        dsys *y = r->bad ? NULL : get_sys(c, id, ks);
        tvf_cdm *m = y ? ensure_cdm(y, err, sizeof(err)) : NULL;
        dsess *s = m ? calloc(1, sizeof(*s)) : NULL;
        if (s) { /* register beforehand: the keys arrive during loading */
            s->id = sess;
            s->gid = y->gid;
            snprintf(s->sid, sizeof(s->sid), "%s", sid);
            pthread_mutex_lock(&D.lock);
            s->next = c->sess;
            c->sess = s;
            pthread_mutex_unlock(&D.lock);
        }
        int rc = s ? cdm_load_session(m, sid, err, sizeof(err)) : -1;
        mt_log("[eme] load session %s: %s", sid, rc ? err : "ok");
        pthread_mutex_lock(&D.lock);
        if (s) s->open = !rc;
        if (s) flush_pending(y, s);
        pthread_mutex_unlock(&D.lock);
        reply(c, req, rc ? 1 : 0, rc ? err : "");
        return;
    }
    case D_UPDATE:
    case D_REMOVE:
    case D_SCLOSE: {
        req = r_u32(r);
        uint32_t sess = r_u32(r), n = 0;
        const uint8_t *d = type == D_UPDATE ? r_bytes(r, &n) : NULL;
        tvf_cdm *m = NULL;
        pthread_mutex_lock(&D.lock);
        dsess *s = r->bad ? NULL : sess_by_id(c, sess);
        dsys *y = s ? sys_by_gid(s->gid) : NULL;
        if (y) m = y->cdm;
        if (s) snprintf(sid, sizeof(sid), "%s", s->sid);
        pthread_mutex_unlock(&D.lock);
        int rc = -1;
        if (!m) snprintf(err, sizeof(err), "unknown session");
        else if (type == D_UPDATE) rc = cdm_update_session(m, sid, d, n, err, sizeof(err));
        else if (type == D_REMOVE) rc = cdm_remove_session(m, sid, err, sizeof(err));
        else rc = cdm_close_session(m, sid, err, sizeof(err));
        if (type != D_UPDATE || rc) mt_log("[eme] %s %s: %s", type == D_UPDATE ? "license" : type == D_REMOVE ? "remove" : "close",
                                          m ? sid : "?", rc ? err : "ok");
        if (type == D_SCLOSE) {
            pthread_mutex_lock(&D.lock);
            if ((s = sess_by_id(c, sess))) s->open = 0;
            pthread_mutex_unlock(&D.lock);
        }
        reply(c, req, rc ? 1 : 0, rc ? err : "");
        return;
    }
    case D_RELEASE: {
        uint32_t sess = r_u32(r);
        tvf_cdm *m = NULL;
        int open = 0;
        pthread_mutex_lock(&D.lock);
        for (dsess **pp = &c->sess; *pp; pp = &(*pp)->next)
            if ((*pp)->id == sess) {
                dsess *s = *pp;
                *pp = s->next;
                dsys *y = sys_by_gid(s->gid);
                m = y ? y->cdm : NULL;
                open = s->open;
                snprintf(sid, sizeof(sid), "%s", s->sid);
                free(s);
                break;
            }
        pthread_mutex_unlock(&D.lock);
        if (m && open) cdm_close_session(m, sid, err, sizeof(err));
        return;
    }
    default:
        return;
    }
}

void webdrm_serve(int fd) {
    dconn *c = calloc(1, sizeof(*c));
    if (!c) {
        close(fd);
        return;
    }
    c->fd = fd;
    pthread_mutex_init(&c->wlock, NULL);
    mt_log("[eme] client connected");
    for (;;) {
        uint32_t h[2];
        size_t got = 0;
        while (got < sizeof(h)) {
            ssize_t k = read(fd, (char *) h + got, sizeof(h) - got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) goto out;
            got += (size_t) k;
        }
        if (h[1] > (4u << 20)) break;
        uint8_t *b = malloc(h[1] ? h[1] : 1);
        if (!b) break;
        for (got = 0; got < h[1];) {
            ssize_t k = read(fd, b + got, h[1] - got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) {
                free(b);
                goto out;
            }
            got += (size_t) k;
        }
        rd r = {b, b + h[1], 0};
        handle(c, h[0], &r);
        free(b);
    }
out:
    mt_log("[eme] client disconnected");
    /* systems out of the list (after that no event finds them any more), then close */
    dsys *mine = NULL;
    pthread_mutex_lock(&D.lock);
    for (dsys **pp = &D.all; *pp;) {
        dsys *y = *pp;
        if (y->conn == c) {
            *pp = y->next;
            y->next = mine;
            mine = y;
        } else {
            pp = &y->next;
        }
    }
    while (c->sess) {
        dsess *s = c->sess;
        c->sess = s->next;
        free(s);
    }
    pthread_mutex_unlock(&D.lock);
    while (mine) {
        dsys *y = mine;
        mine = y->next;
        close_sys(y);
    }
    pthread_mutex_destroy(&c->wlock);
    close(fd);
    free(c);
}
