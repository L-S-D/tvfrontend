/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mdns.h"

#include "log.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MDNS_PORT 5353
#define MAX_SVC 4
#define TTL_LONG 4500 /* PTR, TXT (RFC 6762 10) */
#define TTL_HOST 120  /* SRV, A */
#define T_A 1
#define T_PTR 12
#define T_TXT 16
#define T_SRV 33
#define T_ANY 255
#define CACHE_FLUSH 0x8000

static char g_inst[64], g_host[64];
static struct in_addr g_ip;
static struct {
    char type[64]; /* "_xbmc-jsonrpc-h._tcp" */
    int port;
} g_svc[MAX_SVC];
static int g_nsvc, g_fd = -1;
static unsigned g_flush = CACHE_FLUSH; /* 0 in unicast responses to simple resolvers (RFC 6762 6.7) */

/* ---------------- Building messages ---------------- */

typedef struct {
    uint8_t b[1500];
    size_t n;
    int over;
} pkt;

static void put(pkt *p, const void *d, size_t n) {
    if (p->n + n > sizeof(p->b)) {
        p->over = 1;
        return;
    }
    memcpy(p->b + p->n, d, n);
    p->n += n;
}

static void put16(pkt *p, unsigned v) {
    uint8_t b[2] = {(uint8_t) (v >> 8), (uint8_t) v};
    put(p, b, 2);
}

static void put32(pkt *p, uint32_t v) {
    uint8_t b[4] = {(uint8_t) (v >> 24), (uint8_t) (v >> 16), (uint8_t) (v >> 8), (uint8_t) v};
    put(p, b, 4);
}

static void put_label(pkt *p, const char *s, size_t n) {
    if (n > 63) n = 63;
    uint8_t l = (uint8_t) n;
    put(p, &l, 1);
    put(p, s, n);
}

/* name from dot-separated labels; prefix (may contain dots) as one label in front */
static void put_name(pkt *p, const char *prefix, const char *dotted) {
    if (prefix) put_label(p, prefix, strlen(prefix));
    for (const char *s = dotted; *s;) {
        const char *d = strchr(s, '.');
        size_t n = d ? (size_t) (d - s) : strlen(s);
        put_label(p, s, n);
        s += n + (d ? 1 : 0);
    }
    uint8_t z = 0;
    put(p, &z, 1);
}

/* header of a record; the data length field is filled in afterwards */
static size_t rr(pkt *p, const char *prefix, const char *name, unsigned type, unsigned cls, uint32_t ttl) {
    put_name(p, prefix, name);
    put16(p, type);
    put16(p, cls);
    put32(p, ttl);
    put16(p, 0);
    return p->n;
}

static void rr_end(pkt *p, size_t at) {
    if (p->over) return;
    size_t len = p->n - at;
    p->b[at - 2] = (uint8_t) (len >> 8);
    p->b[at - 1] = (uint8_t) len;
}

static void svc_full(int i, char *out, size_t n) { snprintf(out, n, "%.63s.local", g_svc[i].type); }

static void add_ptr(pkt *p, int i, uint32_t ttl) {
    char t[96];
    svc_full(i, t, sizeof(t));
    size_t at = rr(p, NULL, t, T_PTR, 1, ttl);
    put_name(p, g_inst, t);
    rr_end(p, at);
}

static void add_srv(pkt *p, int i, uint32_t ttl) {
    char t[96];
    svc_full(i, t, sizeof(t));
    size_t at = rr(p, g_inst, t, T_SRV, 1 | g_flush, ttl);
    put16(p, 0);
    put16(p, 0);
    put16(p, (unsigned) g_svc[i].port);
    put_name(p, NULL, g_host);
    rr_end(p, at);
}

static void add_txt(pkt *p, int i, uint32_t ttl) {
    char t[96];
    svc_full(i, t, sizeof(t));
    size_t at = rr(p, g_inst, t, T_TXT, 1 | g_flush, ttl);
    put_label(p, "txtvers=1", 9);
    rr_end(p, at);
}

static void add_a(pkt *p, uint32_t ttl) {
    size_t at = rr(p, NULL, g_host, T_A, 1 | g_flush, ttl);
    put(p, &g_ip.s_addr, 4);
    rr_end(p, at);
}

static void header(pkt *p, unsigned id, unsigned qd, unsigned an, unsigned ar) {
    p->n = 0;
    p->over = 0;
    put16(p, id);
    put16(p, 0x8400); /* response, authoritative */
    put16(p, qd);
    put16(p, an);
    put16(p, 0);
    put16(p, ar);
}

static void set_counts(pkt *p, unsigned an, unsigned ar) {
    p->b[6] = (uint8_t) (an >> 8);
    p->b[7] = (uint8_t) an;
    p->b[10] = (uint8_t) (ar >> 8);
    p->b[11] = (uint8_t) ar;
}

static void send_to(const pkt *p, const struct sockaddr_in *to) {
    if (p->over) return;
    sendto(g_fd, p->b, p->n, 0, (const struct sockaddr *) to, sizeof(*to));
}

static struct sockaddr_in mcast(void) {
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, "224.0.0.251", &a.sin_addr);
    return a;
}

static void announce(void) {
    pkt p;
    header(&p, 0, 0, 0, 0);
    unsigned an = 0;
    for (int i = 0; i < g_nsvc; i++) {
        add_ptr(&p, i, TTL_LONG);
        add_srv(&p, i, TTL_HOST);
        add_txt(&p, i, TTL_LONG);
        an += 3;
    }
    add_a(&p, TTL_HOST);
    set_counts(&p, an + 1, 0);
    struct sockaddr_in to = mcast();
    send_to(&p, &to);
}

/* ---------------- Reading queries ---------------- */

/* name at *off (with pointers) as "a.b.c" into out (case is preserved);
 * *off afterwards points past the name. 0 = ok */
static int read_name(const uint8_t *m, size_t len, size_t *off, char *out, size_t n) {
    size_t o = *off, w = 0;
    int jumped = 0, hops = 0;
    out[0] = 0;
    for (;;) {
        if (o >= len || hops > 16) return -1;
        uint8_t l = m[o];
        if ((l & 0xC0) == 0xC0) {
            if (o + 1 >= len) return -1;
            size_t to = ((size_t) (l & 0x3F) << 8) | m[o + 1];
            if (!jumped) *off = o + 2;
            jumped = 1;
            o = to;
            hops++;
            continue;
        }
        if (!l) {
            if (!jumped) *off = o + 1;
            break;
        }
        if (o + 1 + l > len) return -1;
        if (w && w + 1 < n) out[w++] = '.';
        for (size_t i = 0; i < l && w + 1 < n; i++) out[w++] = (char) m[o + 1 + i];
        out[w] = 0;
        o += 1u + l;
    }
    return 0;
}

static int is_inst(const char *name, int i) {
    char full[200];
    snprintf(full, sizeof(full), "%.63s.%.63s.local", g_inst, g_svc[i].type);
    return !strcasecmp(name, full);
}

static void handle(const uint8_t *m, size_t len, const struct sockaddr_in *from) {
    if (len < 12 || (m[2] & 0x80)) return; /* queries only */
    unsigned id = (unsigned) (m[0] << 8 | m[1]);
    unsigned qd = (unsigned) (m[4] << 8 | m[5]);
    int legacy = ntohs(from->sin_port) != MDNS_PORT; /* simple resolver: unicast response */
    pkt p;
    header(&p, legacy ? id : 0, 0, 0, 0);
    size_t qstart = 12, off = 12;
    unsigned an = 0, nq = 0;
    int want_ptr[MAX_SVC] = {0}, want_inst[MAX_SVC] = {0}, want_a = 0, want_meta = 0;
    for (unsigned q = 0; q < qd; q++) {
        char name[256];
        if (read_name(m, len, &off, name, sizeof(name)) != 0 || off + 4 > len) return;
        unsigned type = (unsigned) (m[off] << 8 | m[off + 1]);
        off += 4;
        for (int i = 0; i < g_nsvc; i++) {
            char t[96];
            svc_full(i, t, sizeof(t));
            if (!strcasecmp(name, t) && (type == T_PTR || type == T_ANY)) want_ptr[i] = 1;
            if (is_inst(name, i) && (type == T_SRV || type == T_TXT || type == T_ANY)) want_inst[i] = 1;
        }
        if (!strcasecmp(name, g_host) && (type == T_A || type == T_ANY)) want_a = 1;
        if (!strcasecmp(name, "_services._dns-sd._udp.local") && (type == T_PTR || type == T_ANY)) want_meta = 1;
        nq++;
    }
    int any = want_a || want_meta;
    for (int i = 0; i < g_nsvc; i++) any |= want_ptr[i] | want_inst[i];
    if (!any) return;
    if (legacy) { /* echo the question(s) */
        put(&p, m + qstart, off - qstart);
        p.b[4] = (uint8_t) (nq >> 8);
        p.b[5] = (uint8_t) nq;
    }
    uint32_t tl = legacy ? 10 : TTL_LONG, th = legacy ? 10 : TTL_HOST;
    g_flush = legacy ? 0 : CACHE_FLUSH;
    if (want_meta)
        for (int i = 0; i < g_nsvc; i++) {
            char t[96];
            svc_full(i, t, sizeof(t));
            size_t at = rr(&p, NULL, "_services._dns-sd._udp.local", T_PTR, 1, tl);
            put_name(&p, NULL, t);
            rr_end(&p, at);
            an++;
        }
    for (int i = 0; i < g_nsvc; i++) {
        if (want_ptr[i]) {
            add_ptr(&p, i, tl);
            an++;
        }
        if (want_ptr[i] || want_inst[i]) { /* add SRV/TXT: saves follow-up queries */
            add_srv(&p, i, th);
            add_txt(&p, i, tl);
            an += 2;
        }
    }
    add_a(&p, th); /* always: target of the SRV */
    an++;
    set_counts(&p, an, 0);
    g_flush = CACHE_FLUSH;
    if (legacy) send_to(&p, from);
    else {
        struct sockaddr_in to = mcast();
        send_to(&p, &to);
    }
}

static void *mdns_main(void *arg) {
    (void) arg;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int announced = 0;
    for (;;) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long ms = (now.tv_sec - t0.tv_sec) * 1000 + (now.tv_nsec - t0.tv_nsec) / 1000000;
        if (announced < 3 && ms >= announced * 1000L) {
            announce();
            announced++;
        }
        struct pollfd pf = {g_fd, POLLIN, 0};
        int r = poll(&pf, 1, announced < 3 ? 250 : -1);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;
        uint8_t buf[1500];
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(g_fd, buf, sizeof(buf), 0, (struct sockaddr *) &from, &fl);
        if (n > 0) handle(buf, (size_t) n, &from);
    }
    mt_log("[mdns] stopped: %s", strerror(errno));
    return NULL;
}

int mdns_start(const char *instance, const char *ip, const mdns_service *s, int n) {
    if (!instance || !*instance || !ip || inet_pton(AF_INET, ip, &g_ip) != 1) return -1;
    snprintf(g_inst, sizeof(g_inst), "%s", instance);
    char dashed[32];
    snprintf(dashed, sizeof(dashed), "%s", ip);
    for (char *c = dashed; *c; c++)
        if (*c == '.') *c = '-';
    snprintf(g_host, sizeof(g_host), "tvf-%s.local", dashed);
    g_nsvc = 0;
    for (int i = 0; i < n && g_nsvc < MAX_SVC; i++)
        if (s[i].port > 0) {
            snprintf(g_svc[g_nsvc].type, sizeof(g_svc[0].type), "%s", s[i].type);
            g_svc[g_nsvc].port = s[i].port;
            g_nsvc++;
        }
    if (!g_nsvc) return -1;
    g_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (g_fd < 0) return -1;
    int one = 1;
    setsockopt(g_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(g_fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(MDNS_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_fd, (struct sockaddr *) &a, sizeof(a)) != 0) {
        mt_log("[mdns] port %d: %s", MDNS_PORT, strerror(errno));
        close(g_fd);
        g_fd = -1;
        return -1;
    }
    struct ip_mreq mr;
    inet_pton(AF_INET, "224.0.0.251", &mr.imr_multiaddr);
    mr.imr_interface = g_ip;
    if (setsockopt(g_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr, sizeof(mr)) != 0)
        mt_log("[mdns] multicast group: %s", strerror(errno));
    setsockopt(g_fd, IPPROTO_IP, IP_MULTICAST_IF, &g_ip, sizeof(g_ip));
    unsigned char ttl = 255, loop = 0;
    setsockopt(g_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(g_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
    pthread_t t;
    if (pthread_create(&t, NULL, mdns_main, NULL) != 0) return -1;
    pthread_detach(t);
    mt_log("[mdns] \"%s\" as %s (%d services)", g_inst, g_host, g_nsvc);
    return 0;
}
