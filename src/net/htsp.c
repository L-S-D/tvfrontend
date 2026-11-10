/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "htsp.h"

#include "htsmsg.h"
#include "log.h"
#include "sha1.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAX_MSG (64u << 20)       /* largest message (getEvents across all channels: a few MB) */
#define SUB_QUEUE_MAX (8u << 20) /* a subscription may run this far ahead, then the reader thread waits */
#define RCVBUF (1 << 20)         /* start burst of a subscription from the keyframe without congestion */
#define SEQ_ROUTES 32
#define HTSP_QUEUE_DEPTH (16 << 20) /* tvh queueDepth (bytes) of a subscription, about 6 s of UHD */

static int g_queue_depth = HTSP_QUEUE_DEPTH;

void htsp_set_queue_depth(int bytes) { g_queue_depth = bytes; }

static htsp_cfg cfg;
static pthread_mutex_t wmu = PTHREAD_MUTEX_INITIALIZER; /* writing, fd, seq, gen */
static int fd = -1;
static volatile int up;
static int next_seq = 100;
static int gen; /* connection counter */

/* Subscriptions on the connection: their messages (subscriptionId) and the replies
 * to their commands (seq) go raw into the subscription's queue */
typedef struct sqn {
    struct sqn *next;
    unsigned char *body;
    size_t len;
} sqn;

struct htsp_sub {
    int id;
    int gen;       /* connection of the subscription */
    int dead;      /* connection lost */
    int cancelled; /* htsp_sub_cancel */
    sqn *head, *tail;
    size_t bytes;
    pthread_cond_t cond; /* new message, lost, cancelled */
    struct htsp_sub *next;
};

static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER; /* subscriptions, queues, routes; after wmu */
static pthread_cond_t space = PTHREAD_COND_INITIALIZER; /* room in a subscription queue */
static htsp_sub *subs;
static int next_sub = 1;
/* reply with seq -> subscription (sub 0: discard) */
static struct {
    int seq, sub;
} routes[SEQ_ROUTES];

typedef struct qn {
    struct qn *next;
    cJSON *j;
} qn;
static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static qn *qhead, *qtail;

static void push(cJSON *j) {
    qn *n = malloc(sizeof(*n));
    if (!n) {
        cJSON_Delete(j);
        return;
    }
    n->next = NULL;
    n->j = j;
    pthread_mutex_lock(&qmu);
    if (qtail) qtail->next = n;
    else qhead = n;
    qtail = n;
    pthread_mutex_unlock(&qmu);
}

static void push_marker(const char *method) {
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "method", method);
    push(j);
}

cJSON *htsp_next(void) {
    pthread_mutex_lock(&qmu);
    qn *n = qhead;
    if (n) {
        qhead = n->next;
        if (!qhead) qtail = NULL;
    }
    pthread_mutex_unlock(&qmu);
    if (!n) return NULL;
    cJSON *j = n->j;
    free(n);
    return j;
}

int htsp_connected(void) { return up; }

static int send_all(int s, const unsigned char *p, size_t n) {
    while (n) {
        ssize_t w = send(s, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t) w;
    }
    return 0;
}

static int recv_all(int s, unsigned char *p, size_t n) {
    while (n) {
        ssize_t r = recv(s, p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r;
        n -= (size_t) r;
    }
    return 0;
}

static unsigned char *read_body(int s, size_t *len) {
    unsigned char h[4];
    if (recv_all(s, h, 4) != 0) return NULL;
    size_t n = (size_t) h[0] << 24 | (size_t) h[1] << 16 | (size_t) h[2] << 8 | h[3];
    if (n > MAX_MSG) {
        mt_log("[htsp] message too large (%zu bytes)", n);
        return NULL;
    }
    unsigned char *b = malloc(n ? n : 1);
    if (!b) return NULL;
    if (recv_all(s, b, n) != 0) {
        free(b);
        return NULL;
    }
    *len = n;
    return b;
}

/* read one message; NULL = connection lost; *bytes = size */
static cJSON *read_msg(int s, size_t *bytes) {
    unsigned char h[4];
    if (recv_all(s, h, 4) != 0) return NULL;
    size_t len = (size_t) h[0] << 24 | (size_t) h[1] << 16 | (size_t) h[2] << 8 | h[3];
    if (len > MAX_MSG) {
        mt_log("[htsp] message too large (%zu bytes)", len);
        return NULL;
    }
    unsigned char *b = malloc(len ? len : 1);
    if (!b) return NULL;
    if (recv_all(s, b, len) != 0) {
        free(b);
        return NULL;
    }
    cJSON *j = hmsg_parse(b, len);
    free(b);
    if (bytes) *bytes = len + 4;
    if (!j) mt_log("[htsp] message unreadable (%zu bytes)", len);
    return j;
}

static int send_msg(int s, hmsg *m) {
    size_t n;
    const unsigned char *w = hmsg_wire(m, &n);
    return w ? send_all(s, w, n) : -1;
}

/* text from a map; "" if missing */
static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : "";
}

static int dial(const char *host, int port) {
    char ps[8];
    snprintf(ps, sizeof(ps), "%d", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, ps, &hints, &res) != 0 || !res) return -1;
    int s = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s < 0) {
        freeaddrinfo(res);
        return -1;
    }
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)); /* only for hello/authentication */
    if (connect(s, res->ai_addr, res->ai_addrlen) != 0) {
        close(s);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* tvh stays silent when nothing changes: detect a dead connection via keepalive */
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int idle = 60, intvl = 10, cnt = 3;
    setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    return s;
}

/* a reply that refuses (noaccess): 1 = the user (login wrong or missing),
 * 2 = the limit of connections (noaccess with connlimit: no question of user
 * and password), 0 = not refused */
static int refused(const cJSON *r) {
    const cJSON *na = cJSON_GetObjectItemCaseSensitive(r, "noaccess");
    if (!na || !(cJSON_IsTrue(na) || cJSON_GetNumberValue(na) != 0)) return 0;
    return cJSON_GetObjectItemCaseSensitive(r, "connlimit") ? 2 : 1;
}

/* 0 = logged in, 1 = user refused, 2 = connection limit, -1 = no answer */
static int login(int s, const char *user, const char *pass) {
    hmsg m;
    hmsg_init(&m);
    hmsg_str(&m, "method", "hello");
    hmsg_s64(&m, "htspversion", 34);
    hmsg_str(&m, "clientname", "TVF");
    hmsg_str(&m, "clientversion", "1.0");
    hmsg_s64(&m, "seq", 1);
    int r = send_msg(s, &m);
    hmsg_free(&m);
    if (r != 0) return -1;
    cJSON *hello = read_msg(s, NULL);
    if (!hello) return -1;
    unsigned char challenge[64];
    int cl = hmsg_unhex(jstr(hello, "challenge"), challenge, sizeof(challenge));
    mt_log("[htsp] %s %s, HTSP %d", jstr(hello, "servername"), jstr(hello, "serverversion"),
               (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(hello, "htspversion")));
    cJSON_Delete(hello);
    if (!user || !user[0]) return 0;
    if (cl < 0) cl = 0;
    uint8_t digest[20];
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, pass, strlen(pass));
    sha1_update(&c, challenge, (size_t) cl);
    sha1_final(&c, digest);
    hmsg_init(&m);
    hmsg_str(&m, "method", "authenticate");
    hmsg_str(&m, "username", user);
    hmsg_bin(&m, "digest", digest, sizeof(digest));
    hmsg_s64(&m, "seq", 2);
    r = send_msg(s, &m);
    hmsg_free(&m);
    if (r != 0) return -1;
    cJSON *a = read_msg(s, NULL);
    if (!a) return -1;
    int denied = refused(a);
    cJSON_Delete(a);
    if (denied) mt_log("[htsp] login as %s rejected%s", user, denied == 2 ? ": connection limit" : "");
    return denied;
}

/* read method, subscriptionId and seq without parsing the whole message */
static void peek(const unsigned char *b, size_t len, char *method, size_t mn, int64_t *sub, int64_t *seq) {
    size_t pos = 0;
    hmsg_field f;
    method[0] = 0;
    *sub = *seq = -1;
    while (hmsg_next(b, len, &pos, &f) == 1) {
        if (hmsg_is(&f, "method")) snprintf(method, mn, "%.*s", (int) f.dlen, (const char *) f.data);
        else if (hmsg_is(&f, "subscriptionId")) *sub = hmsg_s64_of(&f);
        else if (hmsg_is(&f, "seq")) *seq = hmsg_s64_of(&f);
    }
}

static htsp_sub *find_sub(int id) { /* under smu */
    htsp_sub *x = subs;
    while (x && x->id != id) x = x->next;
    return x;
}

static void add_route(int seq, int sub) { /* under smu */
    for (int i = 0; i < SEQ_ROUTES; i++)
        if (!routes[i].seq) {
            routes[i].seq = seq;
            routes[i].sub = sub;
            return;
        }
}

/* Queue a subscription's message (or a reply to one) there; waits as long as
 * its queue is full. 1 = done (body no longer belongs to the
 * caller; messages of old subscriptions are discarded), 0 = for the main thread */
static int route_sub(unsigned char *body, size_t len) {
    char method[32];
    int64_t sid, seq;
    peek(body, len, method, sizeof(method), &sid, &seq);
    int target = -1;
    pthread_mutex_lock(&smu);
    if (sid >= 0) {
        target = (int) sid;
    } else if (!method[0] && seq >= 0) {
        for (int i = 0; i < SEQ_ROUTES; i++)
            if (routes[i].seq && routes[i].seq == seq) {
                target = routes[i].sub;
                routes[i].seq = 0;
                break;
            }
    }
    if (target < 0) {
        pthread_mutex_unlock(&smu);
        return 0;
    }
    for (;;) {
        htsp_sub *x = find_sub(target);
        if (!x || x->dead || x->cancelled) break;
        if (x->bytes < SUB_QUEUE_MAX || !x->head) {
            sqn *n = malloc(sizeof(*n));
            if (!n) break;
            n->next = NULL;
            n->body = body;
            n->len = len;
            if (x->tail) x->tail->next = n;
            else x->head = n;
            x->tail = n;
            x->bytes += len;
            pthread_cond_signal(&x->cond);
            pthread_mutex_unlock(&smu);
            return 1;
        }
        pthread_cond_wait(&space, &smu); /* the player catches up reading */
    }
    pthread_mutex_unlock(&smu);
    free(body);
    return 1;
}

static void *htsp_main(void *arg) {
    (void) arg;
    int backoff = 1;
    for (;;) {
        int s = dial(cfg.host, cfg.port);
        if (s < 0) {
            mt_log("[htsp] %s:%d unreachable - retrying in %d s", cfg.host, cfg.port, backoff);
            sleep((unsigned) backoff);
            backoff = backoff < 30 ? backoff * 2 : 30;
            continue;
        }
        int h = login(s, cfg.user, cfg.pass);
        if (h == 1) push_marker("_noaccess");
        if (h != 0) {
            close(s);
            int wait = h > 0 ? 60 : backoff;
            sleep((unsigned) wait);
            backoff = backoff < 30 ? backoff * 2 : 30;
            continue;
        }
        struct timeval tv = {0, 0}; /* from now on no read timeout: tvh may stay silent */
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        int rcv = RCVBUF;
        setsockopt(s, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof(rcv));
        push_marker("_sync");
        hmsg m;
        hmsg_init(&m);
        hmsg_str(&m, "method", "enableAsyncMetadata");
        hmsg_s64(&m, "epg", 1);
        hmsg_s64(&m, "epgMaxTime", (int64_t) time(NULL) + cfg.epg_window);
        hmsg_str(&m, "language", "ger");
        hmsg_s64(&m, "seq", 3);
        pthread_mutex_lock(&wmu);
        int r = send_msg(s, &m);
        if (r == 0) {
            fd = s;
            up = 1;
            gen++;
        }
        pthread_mutex_unlock(&wmu);
        hmsg_free(&m);
        if (r != 0) {
            close(s);
            continue;
        }
        backoff = 1;
        size_t total = 0, n = 0;
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int synced = 0;
        for (;;) {
            size_t len = 0;
            unsigned char *body = read_body(s, &len);
            if (!body) break;
            total += len + 4;
            n++;
            if (route_sub(body, len)) continue;
            cJSON *j = hmsg_parse(body, len);
            free(body);
            if (!j) {
                mt_log("[htsp] message unreadable (%zu bytes)", len);
                continue;
            }
            /* the reply to enableAsyncMetadata (seq 3): without rights (e.g.
             * anonymous where tvh wants a login) tvh only says noaccess */
            if (!*jstr(j, "method") && cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "seq")) == 3 &&
                refused(j) == 1) {
                mt_log("[htsp] no access as %s", cfg.user[0] ? cfg.user : "anonymous");
                cJSON_Delete(j);
                push_marker("_noaccess");
                continue;
            }
            if (!synced && !strcmp(jstr(j, "method"), "initialSyncCompleted")) {
                struct timespec t1;
                clock_gettime(CLOCK_MONOTONIC, &t1);
                synced = 1;
                mt_log("[htsp] sync done: %zu messages, %.1f MB in %.0f ms", n, total / 1048576.0,
                       (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_nsec - t0.tv_nsec) / 1e6);
            }
            push(j);
        }
        pthread_mutex_lock(&wmu);
        up = 0;
        fd = -1;
        pthread_mutex_unlock(&wmu);
        close(s);
        /* the connection's subscriptions are over */
        pthread_mutex_lock(&smu);
        for (htsp_sub *x = subs; x; x = x->next) {
            x->dead = 1;
            pthread_cond_broadcast(&x->cond);
        }
        memset(routes, 0, sizeof(routes));
        pthread_cond_broadcast(&space);
        pthread_mutex_unlock(&smu);
        mt_log("[htsp] connection lost - reconnecting in %d s", backoff);
        push_marker("_down");
        sleep((unsigned) backoff);
        backoff = backoff < 30 ? backoff * 2 : 30;
    }
    return NULL;
}

void htsp_start(const htsp_cfg *c) {
    cfg = *c;
    if (!cfg.port) cfg.port = 9982;
    if (cfg.epg_window <= 0) cfg.epg_window = 12 * 3600;
    pthread_t t;
    if (pthread_create(&t, NULL, htsp_main, NULL) == 0) pthread_detach(t);
}

int htsp_request(const char *method, const cJSON *args) {
    hmsg m;
    hmsg_init(&m);
    hmsg_str(&m, "method", method);
    for (const cJSON *a = args ? args->child : NULL; a; a = a->next) {
        if (cJSON_IsNumber(a)) hmsg_s64(&m, a->string, (int64_t) a->valuedouble);
        else if (cJSON_IsString(a)) hmsg_str(&m, a->string, a->valuestring);
        else if (cJSON_IsBool(a)) hmsg_s64(&m, a->string, cJSON_IsTrue(a) ? 1 : 0);
    }
    pthread_mutex_lock(&wmu);
    int seq = -1;
    if (up && fd >= 0) {
        seq = next_seq++;
        hmsg_s64(&m, "seq", seq);
        if (send_msg(fd, &m) != 0) {
            seq = -1;
            shutdown(fd, SHUT_RDWR); /* the reader thread notices the disconnect */
        }
    }
    pthread_mutex_unlock(&wmu);
    hmsg_free(&m);
    return seq;
}

/* ---------------- Subscriptions (player) ---------------- */

htsp_sub *htsp_sub_open(uint32_t channel, int timeshift_s, const char *profile, int *id) {
    htsp_sub *x = calloc(1, sizeof(*x));
    if (!x) return NULL;
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&x->cond, &ca);
    pthread_condattr_destroy(&ca);
    pthread_mutex_lock(&wmu);
    if (!up || fd < 0) {
        pthread_mutex_unlock(&wmu);
        pthread_cond_destroy(&x->cond);
        free(x);
        return NULL;
    }
    int seq = next_seq++;
    pthread_mutex_lock(&smu);
    x->id = next_sub++;
    x->gen = gen;
    x->next = subs;
    subs = x;
    add_route(seq, x->id);
    pthread_mutex_unlock(&smu);
    hmsg m;
    hmsg_init(&m);
    hmsg_str(&m, "method", "subscribe");
    hmsg_s64(&m, "subscriptionId", x->id);
    hmsg_s64(&m, "channelId", (int64_t) channel);
    hmsg_s64(&m, "90khz", 1);
    if (timeshift_s > 0) hmsg_s64(&m, "timeshiftPeriod", timeshift_s);
    /* tvh drops B and P frames once more than this many bytes wait in its queue for
     * us (default 500 kB: a fifth of a second of UHD; a source that delivers in bursts
     * overflows that every time) */
    if (g_queue_depth > 0) hmsg_s64(&m, "queueDepth", g_queue_depth);
    if (profile && profile[0]) hmsg_str(&m, "profile", profile);
    hmsg_s64(&m, "seq", seq);
    int r = send_msg(fd, &m);
    if (r != 0) shutdown(fd, SHUT_RDWR); /* the reader thread notices the disconnect */
    pthread_mutex_unlock(&wmu);
    hmsg_free(&m);
    if (id) *id = x->id;
    if (r != 0) {
        htsp_sub_close(x);
        return NULL;
    }
    return x;
}

unsigned char *htsp_sub_read(htsp_sub *x, size_t *len, int timeout_ms, int *gone) {
    struct timespec dl;
    if (timeout_ms >= 0) {
        clock_gettime(CLOCK_MONOTONIC, &dl);
        dl.tv_sec += timeout_ms / 1000;
        dl.tv_nsec += (long) (timeout_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000L;
        }
    }
    pthread_mutex_lock(&smu);
    while (!x->head && !x->dead && !x->cancelled) {
        if (timeout_ms < 0) pthread_cond_wait(&x->cond, &smu);
        else if (pthread_cond_timedwait(&x->cond, &smu, &dl) == ETIMEDOUT) break;
    }
    sqn *n = x->head;
    if (n) {
        x->head = n->next;
        if (!x->head) x->tail = NULL;
        x->bytes -= n->len;
        pthread_cond_broadcast(&space);
    }
    *gone = !n && (x->dead || x->cancelled);
    pthread_mutex_unlock(&smu);
    if (!n) return NULL;
    unsigned char *b = n->body;
    *len = n->len;
    free(n);
    return b;
}

void htsp_sub_cancel(int id) {
    pthread_mutex_lock(&smu);
    htsp_sub *x = find_sub(id);
    if (x) {
        x->cancelled = 1;
        pthread_cond_broadcast(&x->cond);
        pthread_cond_broadcast(&space);
    }
    pthread_mutex_unlock(&smu);
}

int htsp_sub_cmd(int id, hmsg *m) {
    int r = -1;
    pthread_mutex_lock(&wmu);
    pthread_mutex_lock(&smu);
    htsp_sub *x = find_sub(id);
    int ok = x && !x->dead && x->gen == gen && up && fd >= 0;
    int seq = ok ? next_seq++ : -1;
    if (ok) add_route(seq, 0); /* discard the reply */
    pthread_mutex_unlock(&smu);
    if (ok) {
        hmsg_s64(m, "subscriptionId", id);
        hmsg_s64(m, "seq", seq);
        r = send_msg(fd, m);
        if (r != 0) shutdown(fd, SHUT_RDWR);
    }
    pthread_mutex_unlock(&wmu);
    return r;
}

void htsp_sub_close(htsp_sub *x) {
    if (!x) return;
    pthread_mutex_lock(&smu);
    for (htsp_sub **pp = &subs; *pp; pp = &(*pp)->next)
        if (*pp == x) {
            *pp = x->next;
            break;
        }
    for (int i = 0; i < SEQ_ROUTES; i++)
        if (routes[i].seq && routes[i].sub == x->id) routes[i].seq = 0;
    int live = !x->dead;
    while (x->head) {
        sqn *n = x->head;
        x->head = n->next;
        free(n->body);
        free(n);
    }
    pthread_cond_broadcast(&space);
    pthread_mutex_unlock(&smu);
    if (live) { /* tvh ends the subscription; the reader thread discards late packets */
        pthread_mutex_lock(&wmu);
        if (up && fd >= 0 && gen == x->gen) {
            int seq = next_seq++;
            pthread_mutex_lock(&smu);
            add_route(seq, 0);
            pthread_mutex_unlock(&smu);
            hmsg m;
            hmsg_init(&m);
            hmsg_str(&m, "method", "unsubscribe");
            hmsg_s64(&m, "subscriptionId", x->id);
            hmsg_s64(&m, "seq", seq);
            if (send_msg(fd, &m) != 0) shutdown(fd, SHUT_RDWR);
            hmsg_free(&m);
        }
        pthread_mutex_unlock(&wmu);
    }
    pthread_cond_destroy(&x->cond);
    free(x);
}
