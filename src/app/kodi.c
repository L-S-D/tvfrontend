/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE /* memmem, accept4 */
#include "kodi.h"

#include "cJSON.h"
#include "http.h"
#include "keys.h"
#include "log.h"
#include "md5.h"
#include "mdns.h"
#include "prefs.h"
#include "recplay.h"
#include "tvrec.h"
#include "tvsrc.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
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

#define MAX_CLIENTS 24
#define MAX_REQUEST (256 * 1024)
#define MAX_HEADER (16 * 1024)
#define MAX_OUT (32 << 20) /* uncollected replies per client */
#define MAX_JOBS 256       /* unprocessed requests (all clients) */
#define MAX_IMAGE (4 * 1024 * 1024)
#define MAX_IMAGE_THREADS 6
#define STATE_EVERY_MS 300
#define KODI_MAJOR 12 /* Kore copes with this */
#define MAX_TRUSTED 16
#define TRUSTED_KEY "kore.trusted"

/* ======================== Queues ======================== */

typedef struct msg {
    struct msg *next;
    uint32_t cid; /* client; 0 = to all TCP clients (notification) */
    int tcp;
    int ro; /* came via GET: read-only methods only */
    char *text;
} msg;

static pthread_mutex_t qmu = PTHREAD_MUTEX_INITIALIZER;
static msg *jobs, *jobs_tail;       /* network -> main thread */
static msg *replies, *replies_tail; /* main thread -> network */
static int wake_pipe[2] = {-1, -1};
static void (*wake_main)(void);
static volatile int n_tcp; /* connected TCP clients (notifications are worthwhile) */
static volatile int n_jobs; /* length of jobs */

static int push(msg **head, msg **tail, uint32_t cid, int tcp, int ro, char *text) {
    msg *m = text ? calloc(1, sizeof(*m)) : NULL;
    if (!m) {
        free(text);
        return 0;
    }
    m->cid = cid;
    m->tcp = tcp;
    m->ro = ro;
    m->text = text;
    pthread_mutex_lock(&qmu);
    if (*tail) (*tail)->next = m;
    else *head = m;
    *tail = m;
    pthread_mutex_unlock(&qmu);
    return 1;
}

static msg *pop(msg **head, msg **tail) {
    pthread_mutex_lock(&qmu);
    msg *m = *head;
    if (m) {
        *head = m->next;
        if (!*head) *tail = NULL;
    }
    pthread_mutex_unlock(&qmu);
    return m;
}

/* request to the main thread; 0 = too many pending (or out of memory) */
static int job(uint32_t cid, int tcp, int ro, char *text) {
    if (__atomic_add_fetch(&n_jobs, 1, __ATOMIC_RELAXED) > MAX_JOBS) {
        __atomic_sub_fetch(&n_jobs, 1, __ATOMIC_RELAXED);
        free(text);
        return 0;
    }
    if (!push(&jobs, &jobs_tail, cid, tcp, ro, text)) { /* frees text */
        __atomic_sub_fetch(&n_jobs, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (wake_main) wake_main();
    return 1;
}

static void reply(uint32_t cid, int tcp, char *text) {
    push(&replies, &replies_tail, cid, tcp, 0, text);
    if (wake_pipe[1] >= 0) {
        char b = 1;
        ssize_t r = write(wake_pipe[1], &b, 1);
        (void) r;
    }
}

static kodi_cfg cfg;
static kodi_actions act;

/* ---- Login ---- */
static char auth_token[256]; /* expected Basic token (base64 "user:pass"), "" = no login */
static char auth_tag[33];    /* md5 of the credentials: which ones the list belongs to */
static pthread_mutex_t tmu = PTHREAD_MUTEX_INITIALIZER;
static uint32_t trusted[MAX_TRUSTED]; /* addresses with HTTP login (network byte order) */
static int ntrusted, trusted_dirty;

static void b64(const char *in, char *out, size_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t l = strlen(in), o = 0;
    for (size_t i = 0; i < l && o + 5 < n; i += 3) {
        uint32_t v = (uint32_t) (unsigned char) in[i] << 16;
        if (i + 1 < l) v |= (uint32_t) (unsigned char) in[i + 1] << 8;
        if (i + 2 < l) v |= (unsigned char) in[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = i + 1 < l ? t[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < l ? t[v & 63] : '=';
    }
    out[o] = 0;
}

static int is_trusted(uint32_t ip) {
    pthread_mutex_lock(&tmu);
    int r = 0;
    for (int i = 0; i < ntrusted && !r; i++) r = trusted[i] == ip;
    pthread_mutex_unlock(&tmu);
    return r;
}

static void trust(uint32_t ip) {
    pthread_mutex_lock(&tmu);
    int i = 0;
    while (i < ntrusted && trusted[i] != ip) i++;
    if (i == ntrusted) {
        if (ntrusted == MAX_TRUSTED) memmove(trusted, trusted + 1, sizeof(trusted[0]) * --ntrusted);
        trusted[ntrusted++] = ip;
        trusted_dirty = 1;
        char a[32];
        inet_ntop(AF_INET, &ip, a, sizeof(a));
        mt_log("[kodi] %s signed in (TCP allowed)", a);
    }
    pthread_mutex_unlock(&tmu);
}

/* main thread: load or save the remembered addresses */
static void trusted_load(void) {
    const cJSON *t = prefs_get(TRUSTED_KEY), *ip;
    const cJSON *tag = cJSON_GetObjectItemCaseSensitive(t, "auth");
    if (!cJSON_IsString(tag) || strcmp(tag->valuestring, auth_tag)) return; /* different credentials */
    cJSON_ArrayForEach(ip, cJSON_GetObjectItemCaseSensitive(t, "ips")) {
        struct in_addr a;
        if (cJSON_IsString(ip) && inet_pton(AF_INET, ip->valuestring, &a) == 1 && ntrusted < MAX_TRUSTED)
            trusted[ntrusted++] = a.s_addr;
    }
}

static void trusted_save(void) {
    pthread_mutex_lock(&tmu);
    if (!trusted_dirty) {
        pthread_mutex_unlock(&tmu);
        return;
    }
    trusted_dirty = 0;
    cJSON *t = cJSON_CreateObject();
    cJSON_AddStringToObject(t, "auth", auth_tag);
    cJSON *a = cJSON_AddArrayToObject(t, "ips");
    for (int i = 0; i < ntrusted; i++) {
        char s[32];
        inet_ntop(AF_INET, &trusted[i], s, sizeof(s));
        cJSON_AddItemToArray(a, cJSON_CreateString(s));
    }
    pthread_mutex_unlock(&tmu);
    prefs_set(TRUSTED_KEY, t);
}
static char own_ip[32], own_mac[32]; /* towards tvh; MAC for Wake-on-LAN in Kore */

/* own address (UDP connect sends nothing) and the MAC of this interface */
static void find_self(void) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t) (cfg.tvh_http_port ? cfg.tvh_http_port : 9981));
    socklen_t l = sizeof(a);
    if (fd >= 0 && inet_pton(AF_INET, cfg.tvh_host, &a.sin_addr) == 1 &&
        connect(fd, (struct sockaddr *) &a, sizeof(a)) == 0 && getsockname(fd, (struct sockaddr *) &a, &l) == 0)
        inet_ntop(AF_INET, &a.sin_addr, own_ip, sizeof(own_ip));
    if (fd >= 0) close(fd);
    struct ifaddrs *ifs = NULL, *i;
    char ifname[IF_NAMESIZE] = "";
    if (getifaddrs(&ifs) != 0) return;
    for (i = ifs; i; i = i->ifa_next) {
        char ip[32];
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET &&
            inet_ntop(AF_INET, &((struct sockaddr_in *) i->ifa_addr)->sin_addr, ip, sizeof(ip)) && !strcmp(ip, own_ip))
            snprintf(ifname, sizeof(ifname), "%s", i->ifa_name);
    }
    for (i = ifs; i && ifname[0]; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_PACKET || strcmp(i->ifa_name, ifname)) continue;
        const struct sockaddr_ll *ll = (const struct sockaddr_ll *) i->ifa_addr;
        if (ll->sll_halen == 6)
            snprintf(own_mac, sizeof(own_mac), "%02X:%02X:%02X:%02X:%02X:%02X", ll->sll_addr[0], ll->sll_addr[1],
                     ll->sll_addr[2], ll->sll_addr[3], ll->sll_addr[4], ll->sll_addr[5]);
    }
    freeifaddrs(ifs);
}

/* ======================== Network thread ======================== */

typedef struct {
    int fd;
    int tcp;
    uint32_t id;
    uint32_t ip; /* peer (network byte order) */
    char *in;
    size_t nin, cin;
    char *out;
    size_t nout, oout, cout;
    int close_after; /* HTTP: close after the reply */
    int overflow;    /* does not collect its replies: disconnect */
} client;

static client clients[MAX_CLIENTS];
static int lfd_http = -1, lfd_tcp = -1;
static uint32_t next_cid = 1;
static volatile int image_threads;

static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons((uint16_t) port);
    if (bind(fd, (struct sockaddr *) &a, sizeof(a)) != 0 || listen(fd, 16) != 0) {
        mt_log("[kodi] port %d: %s", port, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static void out_add(client *c, const char *s, size_t n) {
    if (c->nout - c->oout + n > MAX_OUT) {
        c->overflow = 1;
        return;
    }
    if (c->nout + n > c->cout) {
        size_t nc = c->cout ? c->cout : 4096;
        while (nc < c->nout + n) nc *= 2;
        char *no = realloc(c->out, nc);
        if (!no) return;
        c->out = no;
        c->cout = nc;
    }
    memcpy(c->out + c->nout, s, n);
    c->nout += n;
}

/* client gone; keep_fd: the socket already belongs to someone else (image thread) */
static void release(client *c, int keep_fd) {
    if (c->fd < 0) return;
    if (!keep_fd) close(c->fd);
    if (c->tcp) __atomic_sub_fetch(&n_tcp, 1, __ATOMIC_RELAXED);
    free(c->in);
    free(c->out);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static void drop(client *c) { release(c, 0); }

static void http_respond(client *c, int status, const char *type, const char *body, size_t len) {
    char h[256];
    const char *st = status == 200   ? "OK"
                     : status == 400 ? "Bad Request"
                     : status == 403 ? "Forbidden"
                     : status == 404 ? "Not Found"
                     : status == 415 ? "Unsupported Media Type"
                     : status == 431 ? "Request Header Fields Too Large"
                     : status == 503 ? "Service Unavailable"
                                     : "Error";
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", status,
                     st, type, len);
    out_add(c, h, (size_t) n);
    out_add(c, body, len);
    c->close_after = 1;
}

static int hexval(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* decode %XX (and + as space if plus); in place */
static void url_decode(char *s, int plus) {
    char *o = s;
    for (char *i = s; *i; i++) {
        if (*i == '%' && hexval(i[1]) >= 0 && hexval(i[2]) >= 0) {
            *o++ = (char) (hexval(i[1]) * 16 + hexval(i[2]));
            i += 2;
        } else if (*i == '+' && plus) {
            *o++ = ' ';
        } else {
            *o++ = *i;
        }
    }
    *o = 0;
}

static void write_all(int fd, const char *b, size_t n) {
    while (n) {
        ssize_t w = send(fd, b, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return;
        b += w;
        n -= (size_t) w;
    }
}

static void simple(int fd, int status, const char *text) {
    char h[256];
    int n = snprintf(h, sizeof(h),
                     "HTTP/1.1 %d %s\r\nContent-Type: text/plain\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     status, text, strlen(text), text);
    write_all(fd, h, (size_t) n);
}

/* image proxy: /image/<image:// URL, encoded once or several times>. Only the
 * image cache of our own tvh (logos, event images) - not an open proxy */
typedef struct {
    int fd;
    char path[2048];
} imgjob;

static void *image_thread(void *arg) {
    imgjob *j = arg;
    char *u = j->path;
    for (int i = 0; i < 5; i++) {
        if (!strncmp(u, "http://", 7) || !strncmp(u, "https://", 8)) break;
        if (!strncmp(u, "image://", 8)) u += 8;
        url_decode(u, 0);
    }
    while (*u == '/') u++;
    size_t l = strlen(u);
    while (l && u[l - 1] == '/') u[--l] = 0;
    char prefix[160];
    snprintf(prefix, sizeof(prefix), "http://%s:%d/imagecache/", cfg.tvh_host, cfg.tvh_http_port);
    if (strncmp(u, prefix, strlen(prefix)) || strchr(u + strlen(prefix), '/') || strstr(u, "..")) {
        mt_log("[kodi] image refused: %.200s", u);
        simple(j->fd, 403, "Forbidden");
    } else {
        http_conn c;
        if (http_open_quiet(u, cfg.user, cfg.pass, &c) != 0) {
            simple(j->fd, 502, "Bad Gateway");
        } else {
            char *buf = malloc(MAX_IMAGE);
            size_t n = 0;
            ssize_t r;
            while (buf && n < MAX_IMAGE && (r = http_read(&c, buf + n, MAX_IMAGE - n)) > 0) n += (size_t) r;
            char h[320];
            int hn = snprintf(h, sizeof(h),
                              "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                              "Cache-Control: public, max-age=86400\r\nConnection: close\r\n\r\n",
                              c.content_type[0] ? c.content_type : "image/png", n);
            http_close(&c);
            if (buf) {
                write_all(j->fd, h, (size_t) hn);
                write_all(j->fd, buf, n);
            }
            free(buf);
        }
    }
    close(j->fd);
    free(j);
    __atomic_sub_fetch(&image_threads, 1, __ATOMIC_RELAXED);
    return NULL;
}

/* like Kodi: application/json, application/json-rpc, application/jsonrequest (also with ";charset=") */
static int json_type(const char *t) {
    size_t n = strcspn(t, "; \t");
    return (n == 16 && !strncasecmp(t, "application/json", n)) || (n == 20 && !strncasecmp(t, "application/json-rpc", n)) ||
           (n == 23 && !strncasecmp(t, "application/jsonrequest", n));
}

/* request from a browser (with Origin): only from a page in the home network. Under a
 * foreign name (DNS rebinding) a web page would otherwise reach TVF as "same origin" */
static int local_origin(const char *o) {
    const char *h = strstr(o, "://");
    if (!h) return 0; /* also "null" (sandbox, file) */
    h += 3;
    if (*h == '[') return strchr(h, ']') != NULL; /* IPv6 address */
    char host[256];
    size_t n = strcspn(h, ":/");
    if (!n || n >= sizeof(host)) return 0;
    memcpy(host, h, n);
    host[n] = 0;
    struct in_addr a;
    if (inet_pton(AF_INET, host, &a) == 1) {
        uint32_t ip = ntohl(a.s_addr);
        return (ip >> 24) == 10 || (ip >> 24) == 127 || (ip >> 20) == 0xAC1 || (ip >> 16) == 0xC0A8 ||
               (ip >> 16) == 0xA9FE;
    }
    if (!strchr(host, '.')) return 1; /* localhost, host name */
    return n > 6 && !strcasecmp(host + n - 6, ".local");
}

/* complete HTTP request: 1 = client stays in the network thread, 0 = handed off */
static int http_request_done(client *c, const char *method, char *target, const char *ctype, const char *body,
                             size_t blen) {
    if (!strncmp(target, "/jsonrpc", 8) && (target[8] == 0 || target[8] == '?')) {
        if (!strcmp(method, "POST")) {
            /* JSON only: any foreign web page can send a form or text/plain without the
             * browser asking first (for JSON it asks via OPTIONS beforehand, which fails) */
            if (!json_type(ctype)) {
                static uint32_t last_ip;
                static time_t last_t;
                time_t now = time(NULL);
                if (c->ip != last_ip || now - last_t > 60) { /* a client that keeps trying */
                    char a[32];
                    inet_ntop(AF_INET, &c->ip, a, sizeof(a));
                    mt_log("[kodi] %s: POST with Content-Type \"%.60s\" refused (needs application/json)", a, ctype);
                    last_ip = c->ip;
                    last_t = now;
                }
                http_respond(c, 415, "text/plain", "Content-Type must be application/json", 37);
                return 1;
            }
            char *t = malloc(blen + 1);
            if (t) {
                memcpy(t, body, blen);
                t[blen] = 0;
            }
            if (!job(c->id, 0, 0, t)) http_respond(c, 503, "text/plain", "busy", 4);
            return 1;
        }
        char *q = strstr(target, "request=");
        if (q) { /* GET: read-only methods only (an <img> on a web page must not trigger anything) */
            q += 8;
            char *amp = strchr(q, '&');
            if (amp) *amp = 0;
            url_decode(q, 1);
            if (!job(c->id, 0, 1, strdup(q))) http_respond(c, 503, "text/plain", "busy", 4);
            return 1;
        }
        static const char hello[] = "{\"jsonrpc\":\"2.0\",\"result\":\"TVF\"}";
        http_respond(c, 200, "application/json", hello, sizeof(hello) - 1);
        return 1;
    }
    if (!strncmp(target, "/image/", 7) && !strcmp(method, "GET")) {
        if (__atomic_add_fetch(&image_threads, 1, __ATOMIC_RELAXED) > MAX_IMAGE_THREADS) {
            __atomic_sub_fetch(&image_threads, 1, __ATOMIC_RELAXED);
            http_respond(c, 503, "text/plain", "busy", 4);
            return 1;
        }
        imgjob *j = calloc(1, sizeof(*j));
        pthread_t t;
        if (j) {
            j->fd = c->fd;
            snprintf(j->path, sizeof(j->path), "%s", target + 7);
            char *qm = strchr(j->path, '?');
            if (qm) *qm = 0;
            int fl = fcntl(j->fd, F_GETFL);
            fcntl(j->fd, F_SETFL, fl & ~O_NONBLOCK);
            struct timeval tv = {10, 0};
            setsockopt(j->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
            int ok = pthread_create(&t, &at, image_thread, j) == 0;
            pthread_attr_destroy(&at);
            if (ok) {
                release(c, 1); /* the socket now belongs to the image thread */
                return 0;
            }
            free(j);
        }
        __atomic_sub_fetch(&image_threads, 1, __ATOMIC_RELAXED);
        http_respond(c, 503, "text/plain", "busy", 4);
        return 1;
    }
    http_respond(c, 404, "text/plain", "not found", 9);
    return 1;
}

/* value of a header line without leading and trailing whitespace, truncated to n */
static void header_value(const char *v, char *out, size_t n) {
    while (*v == ' ' || *v == '\t') v++;
    size_t l = strcspn(v, "\r\n");
    while (l && (v[l - 1] == ' ' || v[l - 1] == '\t')) l--;
    if (l >= n) l = n - 1;
    memcpy(out, v, l);
    out[l] = 0;
}

/* HTTP: header + body (Content-Length) complete? */
static void http_input(client *c) {
    if (c->close_after) return; /* one request per connection */
    char *end = c->nin >= 4 ? memmem(c->in, c->nin, "\r\n\r\n", 4) : NULL;
    if (!end) {
        if (c->nin > MAX_HEADER) http_respond(c, 431, "text/plain", "header too large", 16);
        return;
    }
    size_t hlen = (size_t) (end - c->in) + 4;
    long long clen = 0;
    char auth[300] = "", ctype[80] = "", origin[300] = "";
    int has_origin = 0;
    for (char *l = c->in; l < end; l = strstr(l, "\r\n") + 2) {
        if (!strncasecmp(l, "Content-Length:", 15)) clen = atoll(l + 15);
        if (!strncasecmp(l, "Content-Type:", 13)) header_value(l + 13, ctype, sizeof(ctype));
        if (!strncasecmp(l, "Origin:", 7)) {
            has_origin = 1;
            header_value(l + 7, origin, sizeof(origin));
        }
        if (!strncasecmp(l, "Authorization:", 14)) {
            const char *v = l + 14;
            while (*v == ' ') v++;
            if (!strncasecmp(v, "Basic ", 6)) {
                v += 6;
                size_t n = strcspn(v, "\r\n");
                if (n < sizeof(auth)) {
                    memcpy(auth, v, n);
                    auth[n] = 0;
                }
            }
        }
        if (!strstr(l, "\r\n")) break;
    }
    if (clen < 0 || clen > MAX_REQUEST) {
        http_respond(c, 400, "text/plain", "bad request", 11);
        return;
    }
    if (c->nin < hlen + (size_t) clen) return;
    char method[8] = "", target[2048] = "";
    if (sscanf(c->in, "%7s %2047s", method, target) != 2) {
        http_respond(c, 400, "text/plain", "bad request", 11);
        return;
    }
    if (has_origin && !local_origin(origin)) {
        mt_log("[kodi] request from web page %.200s refused", origin);
        http_respond(c, 403, "text/plain", "foreign origin", 14);
        return;
    }
    if (auth_token[0]) { /* login like Kodi */
        if (strcmp(auth, auth_token)) {
            static const char deny[] = "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"TVF\"\r\n"
                                       "Content-Type: text/plain\r\nContent-Length: 12\r\nConnection: close\r\n\r\n"
                                       "Unauthorized";
            out_add(c, deny, sizeof(deny) - 1);
            c->close_after = 1;
            if (auth[0]) {
                char a[32];
                inet_ntop(AF_INET, &c->ip, a, sizeof(a));
                mt_log("[kodi] %s: wrong user/password", a);
            }
            return;
        }
        if (!is_trusted(c->ip)) trust(c->ip);
    }
    c->close_after = 2; /* reply pending */
    http_request_done(c, method, target, ctype, c->in + hlen, (size_t) clen);
}

/* TCP: extract complete JSON values ({...} or [...]); < 0 = disconnect */
static int tcp_input(client *c) {
    size_t start = 0;
    for (;;) {
        int depth = 0, instr = 0, esc = 0;
        size_t b = (size_t) -1, i;
        for (i = start; i < c->nin; i++) {
            char ch = c->in[i];
            if (esc) {
                esc = 0;
                continue;
            }
            if (instr) {
                if (ch == '\\') esc = 1;
                else if (ch == '"') instr = 0;
                continue;
            }
            if (!depth && ch != '{' && ch != '[') {
                if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
                return -1; /* not JSON-RPC, e.g. HTTP from a web page: do not search it for JSON */
            }
            if (ch == '"') instr = 1;
            else if (ch == '{' || ch == '[') {
                if (!depth) b = i;
                depth++;
            } else if ((ch == '}' || ch == ']') && depth > 0 && --depth == 0) {
                break;
            }
        }
        if (i >= c->nin || b == (size_t) -1) break;
        size_t n = i + 1 - b;
        char *t = malloc(n + 1);
        if (t) {
            memcpy(t, c->in + b, n);
            t[n] = 0;
        }
        if (!job(c->id, 1, 0, t)) return -2;
        start = i + 1;
    }
    if (start) {
        memmove(c->in, c->in + start, c->nin - start);
        c->nin -= start;
    }
    return c->nin > MAX_REQUEST ? -1 : 0; /* a value over 256 KiB is nonsense */
}

static client *by_id(uint32_t id) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (clients[i].fd >= 0 && clients[i].id == id) return &clients[i];
    return NULL;
}

static void deliver_replies(void) {
    msg *m;
    while ((m = pop(&replies, &replies_tail)) != NULL) {
        if (!m->cid) { /* notification */
            for (int i = 0; i < MAX_CLIENTS; i++)
                if (clients[i].fd >= 0 && clients[i].tcp) {
                    out_add(&clients[i], m->text, strlen(m->text));
                    out_add(&clients[i], "\n", 1);
                }
        } else {
            client *c = by_id(m->cid);
            if (c && c->tcp) {
                out_add(c, m->text, strlen(m->text));
                out_add(c, "\n", 1);
            } else if (c) {
                c->close_after = 0;
                http_respond(c, 200, "application/json", m->text, strlen(m->text));
            }
        }
        free(m->text);
        free(m);
    }
}

static void accept_on(int lfd, int tcp) {
    for (;;) {
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        memset(&peer, 0, sizeof(peer));
        int fd = accept4(lfd, (struct sockaddr *) &peer, &pl, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        /* TCP has no login (as with Kodi): only addresses that have logged in via HTTP */
        if (tcp && auth_token[0] && !is_trusted(peer.sin_addr.s_addr)) {
            char a[32];
            inet_ntop(AF_INET, &peer.sin_addr, a, sizeof(a));
            mt_log("[kodi] TCP from %s refused (no HTTP sign-in yet)", a);
            close(fd);
            continue;
        }
        client *c = NULL;
        for (int i = 0; i < MAX_CLIENTS && !c; i++)
            if (clients[i].fd < 0) c = &clients[i];
        if (!c) {
            close(fd);
            continue;
        }
        memset(c, 0, sizeof(*c));
        c->fd = fd;
        c->tcp = tcp;
        c->ip = peer.sin_addr.s_addr;
        c->id = next_cid++;
        if (!next_cid) next_cid = 1;
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (tcp) {
            __atomic_add_fetch(&n_tcp, 1, __ATOMIC_RELAXED);
            mt_log("[kodi] TCP client connected");
        }
    }
}

static void *net_main(void *arg) {
    (void) arg;
    struct pollfd pf[MAX_CLIENTS + 3];
    int idx[MAX_CLIENTS + 3];
    for (;;) {
        int n = 0;
        if (lfd_http >= 0) pf[n++] = (struct pollfd){lfd_http, POLLIN, 0};
        if (lfd_tcp >= 0) pf[n++] = (struct pollfd){lfd_tcp, POLLIN, 0};
        pf[n++] = (struct pollfd){wake_pipe[0], POLLIN, 0};
        int base = n;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            client *c = &clients[i];
            if (c->fd < 0) continue;
            short ev = POLLIN;
            if (c->nout > c->oout) ev |= POLLOUT;
            idx[n] = i;
            pf[n++] = (struct pollfd){c->fd, ev, 0};
        }
        if (poll(pf, (nfds_t) n, -1) < 0) {
            if (errno == EINTR) continue;
            mt_log("[kodi] poll: %s", strerror(errno));
            return NULL;
        }
        for (int k = 0; k < base; k++) {
            if (!(pf[k].revents & POLLIN)) continue;
            if (pf[k].fd == lfd_http) accept_on(lfd_http, 0);
            else if (pf[k].fd == lfd_tcp) accept_on(lfd_tcp, 1);
            else {
                char b[64];
                while (read(wake_pipe[0], b, sizeof(b)) > 0) {
                }
                deliver_replies();
            }
        }
        for (int k = base; k < n; k++) {
            client *c = &clients[idx[k]];
            if (c->fd != pf[k].fd) continue;
            if (c->overflow) {
                mt_log("[kodi] client does not read its replies - disconnected");
                drop(c);
                continue;
            }
            if (pf[k].revents & (POLLERR | POLLHUP | POLLNVAL) && !(pf[k].revents & POLLIN)) {
                if (c->tcp) mt_log("[kodi] TCP client gone");
                drop(c);
                continue;
            }
            if (pf[k].revents & POLLIN) {
                if (!c->tcp && c->nin > MAX_HEADER + MAX_REQUEST) { /* HTTP: one request per connection */
                    drop(c);
                    continue;
                }
                if (c->nin + 8192 > c->cin) {
                    size_t nc = c->cin ? c->cin * 2 : 16384;
                    char *ni = realloc(c->in, nc);
                    if (!ni) {
                        drop(c);
                        continue;
                    }
                    c->in = ni;
                    c->cin = nc;
                }
                ssize_t r = recv(c->fd, c->in + c->nin, c->cin - c->nin - 1, 0);
                if (r <= 0) {
                    if (r < 0 && (errno == EAGAIN || errno == EINTR)) continue;
                    if (c->tcp) mt_log("[kodi] TCP client disconnected");
                    drop(c);
                    continue;
                }
                c->nin += (size_t) r;
                c->in[c->nin] = 0;
                int r2 = 0;
                if (c->tcp) r2 = tcp_input(c);
                else http_input(c);
                if (r2 < 0) {
                    mt_log("[kodi] TCP client dropped: %s", r2 == -2 ? "too many requests" : "not JSON-RPC");
                    drop(c);
                    continue;
                }
                if (c->fd < 0) continue;
            }
            if (c->nout > c->oout) {
                ssize_t w = send(c->fd, c->out + c->oout, c->nout - c->oout, MSG_NOSIGNAL);
                if (w > 0) c->oout += (size_t) w;
                else if (w < 0 && errno != EAGAIN && errno != EINTR) {
                    drop(c);
                    continue;
                }
                if (c->oout == c->nout) {
                    c->oout = c->nout = 0;
                    if (c->close_after == 1) drop(c);
                }
            }
        }
    }
    return NULL;
}

int kodi_start(const kodi_cfg *c, const kodi_actions *a, void (*wake)(void)) {
    cfg = *c;
    act = *a;
    wake_main = wake;
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
    if (cfg.kore_user[0] || cfg.kore_pass[0]) {
        char up[160];
        snprintf(up, sizeof(up), "%s:%s", cfg.kore_user, cfg.kore_pass);
        b64(up, auth_token, sizeof(auth_token));
        md5_hex(up, auth_tag);
        trusted_load();
    } else if (prefs_get(TRUSTED_KEY)) {
        prefs_set(TRUSTED_KEY, NULL); /* no login, no list */
    }
    if (cfg.http_port > 0) lfd_http = listen_on(cfg.http_port);
    if (cfg.tcp_port > 0) lfd_tcp = listen_on(cfg.tcp_port);
    if (lfd_http < 0 && lfd_tcp < 0) return -1;
    if (pipe2(wake_pipe, O_NONBLOCK | O_CLOEXEC) != 0) return -1;
    pthread_t t;
    if (pthread_create(&t, NULL, net_main, NULL) != 0) return -1;
    pthread_detach(t);
    find_self();
    mt_log("[kodi] \"%s\": HTTP %d, TCP %d (%s, MAC %s), %s", cfg.name, lfd_http >= 0 ? cfg.http_port : 0,
           lfd_tcp >= 0 ? cfg.tcp_port : 0, own_ip[0] ? own_ip : "?", own_mac[0] ? own_mac : "?",
           auth_token[0] ? "with sign-in" : "without sign-in");
    /* Kore discovers TVF like a Kodi */
    mdns_service sv[2] = {{"_xbmc-jsonrpc-h._tcp", lfd_http >= 0 ? cfg.http_port : 0},
                          {"_xbmc-jsonrpc._tcp", lfd_tcp >= 0 ? cfg.tcp_port : 0}};
    if (own_ip[0]) mdns_start(cfg.name, own_ip, sv, 2);
    return 0;
}

/* ======================== JSON-RPC (main thread) ======================== */

typedef struct {
    int code;
    char msg[160];
} rpc_err;

static cJSON defer_marker; /* reply comes later (tvh) */
#define DEFER (&defer_marker)

typedef struct {
    uint32_t cid;
    int tcp;
    cJSON *id;
} later;

static later *cur_later; /* target of a deferred reply */

static cJSON *fail(rpc_err *e, int code, const char *m) {
    e->code = code;
    snprintf(e->msg, sizeof(e->msg), "%s", m);
    return NULL;
}

static const char *pstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) && i->valuestring ? i->valuestring : "";
}

static double pnum(const cJSON *o, const char *k, double dflt) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(i) ? i->valuedouble : dflt;
}

static int has(const cJSON *props, const char *p) {
    const cJSON *i;
    cJSON_ArrayForEach(i, props) if (cJSON_IsString(i) && !strcmp(i->valuestring, p)) return 1;
    return 0;
}

static void local_time(int64_t t, char *out, size_t n) {
    if (!t) {
        out[0] = 0;
        return;
    }
    time_t tt = (time_t) t;
    struct tm lt;
    localtime_r(&tt, &lt);
    strftime(out, n, "%Y-%m-%d %H:%M:%S", &lt);
}

static void add_time(cJSON *o, const char *k, int64_t t) {
    char b[32];
    local_time(t, b, sizeof(b));
    cJSON_AddStringToObject(o, k, b);
}

static cJSON *kodi_time(double secs) {
    long s = secs > 0 ? (long) secs : 0;
    cJSON *t = cJSON_CreateObject();
    cJSON_AddNumberToObject(t, "hours", s / 3600);
    cJSON_AddNumberToObject(t, "minutes", (s % 3600) / 60);
    cJSON_AddNumberToObject(t, "seconds", s % 60);
    cJSON_AddNumberToObject(t, "milliseconds", 0);
    return t;
}

/* image://<encodeURIComponent(url)>/ like Kodi; "" without an image */
static void image_url(const char *url, char *out, size_t n) {
    if (!url || !*url) {
        out[0] = 0;
        return;
    }
    size_t o = (size_t) snprintf(out, n, "image://");
    for (const unsigned char *c = (const unsigned char *) url; *c && o + 4 < n; c++) {
        if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || strchr("-_.!~*'()", *c))
            out[o++] = (char) *c;
        else o += (size_t) snprintf(out + o, n - o, "%%%02X", *c);
    }
    if (o + 1 < n) out[o++] = '/';
    out[o] = 0;
}

static void limits(cJSON *res, int start, int count, int total) {
    cJSON *l = cJSON_AddObjectToObject(res, "limits");
    cJSON_AddNumberToObject(l, "start", start);
    cJSON_AddNumberToObject(l, "end", start + count);
    cJSON_AddNumberToObject(l, "total", total);
}

static void range(const cJSON *params, int total, int *start, int *end) {
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(params, "limits");
    *start = (int) pnum(l, "start", 0);
    *end = (int) pnum(l, "end", total);
    if (*start < 0) *start = 0;
    if (*end > total || *end <= 0) *end = total;
    if (*start > *end) *start = *end;
}

/* ---- Events ---- */

static cJSON *broadcast(const tvsrc_event *e, const cJSON *props) {
    time_t now = time(NULL);
    int active = e->start <= now && now < e->stop;
    int prog = !e->start || !e->stop || now < e->start ? 0 : now >= e->stop ? 100
                                                                  : (int) ((now - e->start) * 100 / (e->stop - e->start));
    uint32_t tid = 0;
    char ar[48] = "";
    int recording = 0;
    int timer = tvrec_timer_of_event(e->id, &tid, ar, sizeof(ar), &recording);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "broadcastid", e->id);
    cJSON_AddStringToObject(o, "label", e->title);
    const char *plot = e->text[0] ? e->text : e->summary;
    if (!props || has(props, "title")) cJSON_AddStringToObject(o, "title", e->title);
    if (!props || has(props, "starttime")) add_time(o, "starttime", e->start);
    if (!props || has(props, "endtime")) add_time(o, "endtime", e->stop);
    if (!props || has(props, "plot")) cJSON_AddStringToObject(o, "plot", plot);
    if (props && has(props, "plotoutline")) cJSON_AddStringToObject(o, "plotoutline", e->summary[0] ? e->summary : plot);
    if (!props || has(props, "episodename")) cJSON_AddStringToObject(o, "episodename", e->subtitle);
    if (!props || has(props, "runtime"))
        cJSON_AddNumberToObject(o, "runtime", e->stop > e->start ? (double) ((e->stop - e->start + 30) / 60) : 0);
    if (!props || has(props, "progress")) cJSON_AddNumberToObject(o, "progress", prog);
    if (!props || has(props, "progresspercentage")) cJSON_AddNumberToObject(o, "progresspercentage", prog);
    if (!props || has(props, "isactive")) cJSON_AddBoolToObject(o, "isactive", active);
    if (props && has(props, "wasactive")) cJSON_AddBoolToObject(o, "wasactive", e->stop && e->stop <= now);
    if (props && has(props, "hastimer")) cJSON_AddBoolToObject(o, "hastimer", timer);
    if (props && has(props, "hastimerrule")) cJSON_AddBoolToObject(o, "hastimerrule", timer && ar[0]);
    if (props && has(props, "hasrecording")) cJSON_AddBoolToObject(o, "hasrecording", recording);
    if (props && has(props, "isrecording")) cJSON_AddBoolToObject(o, "isrecording", recording);
    if (props && (has(props, "thumbnail") || has(props, "art"))) {
        char img[700];
        image_url(e->image, img, sizeof(img));
        if (has(props, "thumbnail")) cJSON_AddStringToObject(o, "thumbnail", img);
        if (has(props, "art")) {
            cJSON *a = cJSON_AddObjectToObject(o, "art");
            if (img[0]) cJSON_AddStringToObject(a, "thumb", img);
        }
    }
    if (props && has(props, "genre")) cJSON_AddItemToObject(o, "genre", cJSON_CreateArray());
    return o;
}

static cJSON *now_next(int num, int next) {
    tvsrc_event e;
    return tvsrc_event_info(num, next, &e) ? broadcast(&e, NULL) : cJSON_CreateObject();
}

/* ---- Channels ---- */

static cJSON *channel_item(int num, const cJSON *props) {
    char name[256] = "", icon[600] = "", img[900];
    tvsrc_channel_meta(num, name, sizeof(name), icon, sizeof(icon));
    image_url(icon, img, sizeof(img));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "channelid", num);
    cJSON_AddStringToObject(o, "label", name);
    if (has(props, "channel")) cJSON_AddStringToObject(o, "channel", name);
    if (has(props, "channelnumber")) cJSON_AddNumberToObject(o, "channelnumber", num);
    if (has(props, "channeltype")) cJSON_AddStringToObject(o, "channeltype", "tv");
    if (has(props, "hidden")) cJSON_AddBoolToObject(o, "hidden", 0);
    if (has(props, "locked")) cJSON_AddBoolToObject(o, "locked", 0);
    if (has(props, "thumbnail")) cJSON_AddStringToObject(o, "thumbnail", img);
    if (has(props, "icon")) cJSON_AddStringToObject(o, "icon", img);
    if (has(props, "lastplayed")) cJSON_AddStringToObject(o, "lastplayed", "");
    if (has(props, "isrecording")) cJSON_AddBoolToObject(o, "isrecording", 0);
    if (has(props, "broadcastnow")) cJSON_AddItemToObject(o, "broadcastnow", now_next(num, 0));
    if (has(props, "broadcastnext")) cJSON_AddItemToObject(o, "broadcastnext", now_next(num, 1));
    return o;
}

static cJSON *get_channel_groups(const cJSON *params) {
    cJSON *res = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(res, "channelgroups");
    const char *type = pstr(params, "channeltype");
    cJSON *all = cJSON_CreateObject();
    int radio = !strcmp(type, "radio");
    cJSON_AddStringToObject(all, "channelgroupid", radio ? "allradio" : "alltv");
    cJSON_AddStringToObject(all, "label", "All channels");
    cJSON_AddStringToObject(all, "channeltype", radio ? "radio" : "tv");
    cJSON_AddItemToArray(arr, all);
    if (!radio) {
        uint32_t ids[256];
        const char *names[256];
        int n = tvsrc_groups(ids, names, 256);
        for (int i = 0; i < n; i++) {
            cJSON *g = cJSON_CreateObject();
            cJSON_AddNumberToObject(g, "channelgroupid", ids[i]);
            cJSON_AddStringToObject(g, "label", names[i]);
            cJSON_AddStringToObject(g, "channeltype", "tv");
            cJSON_AddItemToArray(arr, g);
        }
    }
    int total = cJSON_GetArraySize(arr);
    limits(res, 0, total, total);
    return res;
}

static uint32_t group_of(const cJSON *params, int *radio) {
    const cJSON *g = cJSON_GetObjectItemCaseSensitive(params, "channelgroupid");
    *radio = cJSON_IsString(g) && !strcmp(g->valuestring, "allradio");
    return cJSON_IsNumber(g) ? (uint32_t) g->valuedouble : 0;
}

static cJSON *get_channels(const cJSON *params) {
    int radio;
    uint32_t tag = group_of(params, &radio);
    int total = radio ? 0 : tvsrc_channels(tag, NULL, 0);
    int *nums = total ? malloc(sizeof(int) * (size_t) total) : NULL;
    if (nums) tvsrc_channels(tag, nums, total);
    int start, end;
    range(params, nums ? total : 0, &start, &end);
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(params, "properties");
    cJSON *res = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(res, "channels");
    for (int i = start; i < end; i++) cJSON_AddItemToArray(arr, channel_item(nums[i], props));
    free(nums);
    limits(res, start, end - start, total);
    return res;
}

static cJSON *get_channel_details(const cJSON *params, rpc_err *e) {
    int num = (int) pnum(params, "channelid", 0);
    if (!tvsrc_channel_meta(num, NULL, 0, NULL, 0)) return fail(e, -32602, "Channel not found");
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "channeldetails", channel_item(num, cJSON_GetObjectItemCaseSensitive(params, "properties")));
    return res;
}

#define MAX_EVENTS 2000
static cJSON *get_broadcasts(const cJSON *params) {
    int num = (int) pnum(params, "channelid", 0);
    tvsrc_event *ev = malloc(sizeof(tvsrc_event) * MAX_EVENTS);
    int total = ev ? tvsrc_events(num, ev, MAX_EVENTS) : 0;
    int start, end;
    range(params, total, &start, &end);
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(params, "properties");
    cJSON *res = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(res, "broadcasts");
    cJSON *none = props ? NULL : cJSON_CreateArray();
    for (int i = start; i < end; i++) cJSON_AddItemToArray(arr, broadcast(&ev[i], props ? props : none));
    cJSON_Delete(none);
    free(ev);
    limits(res, start, end - start, total);
    return res;
}

static cJSON *get_broadcast_details(const cJSON *params, rpc_err *e) {
    tvsrc_event ev;
    if (!tvsrc_event_by_id((uint32_t) pnum(params, "broadcastid", 0), &ev, NULL))
        return fail(e, -32602, "Broadcast not found");
    cJSON *res = cJSON_CreateObject();
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(params, "properties");
    cJSON *empty = props ? NULL : cJSON_CreateArray();
    cJSON_AddItemToObject(res, "broadcastdetails", broadcast(&ev, props ? props : empty));
    cJSON_Delete(empty);
    return res;
}

/* ---- Recordings and timers ---- */

static cJSON *recording_item(const cJSON *r, const cJSON *props) {
    uint32_t num = (uint32_t) pnum(r, "num", 0), pnum_ = 0;
    double pos = pnum(r, "position", 0), dur = pnum(r, "duration", 0), ppos = 0;
    if (recplay_info(&pnum_, &ppos, NULL, NULL) && pnum_ == num) pos = ppos;
    char img[900];
    image_url(pstr(r, "icon"), img, sizeof(img));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "recordingid", num);
    cJSON_AddStringToObject(o, "label", pstr(r, "title"));
    if (has(props, "title")) cJSON_AddStringToObject(o, "title", pstr(r, "title"));
    if (has(props, "plot")) cJSON_AddStringToObject(o, "plot", pstr(r, "description"));
    if (has(props, "plotoutline")) cJSON_AddStringToObject(o, "plotoutline", pstr(r, "subtitle"));
    if (has(props, "channel")) cJSON_AddStringToObject(o, "channel", pstr(r, "channelName"));
    if (has(props, "starttime")) add_time(o, "starttime", (int64_t) pnum(r, "start", 0));
    if (has(props, "endtime")) add_time(o, "endtime", (int64_t) pnum(r, "stop", 0));
    if (has(props, "runtime")) cJSON_AddNumberToObject(o, "runtime", (double) (long) (dur + 0.5));
    if (has(props, "lifetime")) cJSON_AddNumberToObject(o, "lifetime", 0);
    if (has(props, "streamurl")) cJSON_AddStringToObject(o, "streamurl", "");
    if (has(props, "file")) {
        char f[128];
        snprintf(f, sizeof(f), "pvr://recordings/tv/active/%s", pstr(r, "id"));
        cJSON_AddStringToObject(o, "file", f);
    }
    if (has(props, "directory")) cJSON_AddStringToObject(o, "directory", pstr(r, "title"));
    if (has(props, "icon")) cJSON_AddStringToObject(o, "icon", img);
    if (has(props, "art")) {
        cJSON *a = cJSON_AddObjectToObject(o, "art");
        if (img[0]) cJSON_AddStringToObject(a, "thumb", img);
    }
    if (has(props, "playcount")) cJSON_AddNumberToObject(o, "playcount", cJSON_IsTrue(cJSON_GetObjectItem(r, "watched")));
    if (has(props, "resume")) {
        cJSON *rs = cJSON_AddObjectToObject(o, "resume");
        cJSON_AddNumberToObject(rs, "position", (double) (long) pos);
        cJSON_AddNumberToObject(rs, "total", (double) (long) dur);
    }
    if (has(props, "isdeleted")) cJSON_AddBoolToObject(o, "isdeleted", 0);
    if (has(props, "radio")) cJSON_AddBoolToObject(o, "radio", 0);
    if (has(props, "isrecording")) cJSON_AddBoolToObject(o, "isrecording", !strcmp(pstr(r, "state"), "recording"));
    if (has(props, "channeluid")) cJSON_AddNumberToObject(o, "channeluid", pnum(r, "channelNumber", 0));
    if (has(props, "episodename")) cJSON_AddStringToObject(o, "episodename", pstr(r, "subtitle"));
    return o;
}

static cJSON *get_recordings(const cJSON *params) {
    cJSON *all = tvrec_list(0);
    int total = cJSON_GetArraySize(all), start, end;
    range(params, total, &start, &end);
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(params, "properties");
    cJSON *res = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(res, "recordings");
    for (int i = start; i < end; i++) cJSON_AddItemToArray(arr, recording_item(cJSON_GetArrayItem(all, i), props));
    cJSON_Delete(all);
    limits(res, start, end - start, total);
    return res;
}

static cJSON *get_recording_details(const cJSON *params, rpc_err *e) {
    cJSON *r = tvrec_recording((uint32_t) pnum(params, "recordingid", 0));
    if (!r) return fail(e, -32602, "Recording not found");
    cJSON *res = cJSON_CreateObject();
    cJSON_AddItemToObject(res, "recordingdetails", recording_item(r, cJSON_GetObjectItemCaseSensitive(params, "properties")));
    cJSON_Delete(r);
    return res;
}

static cJSON *timer_item(const cJSON *t, const cJSON *props) {
    int64_t start = (int64_t) pnum(t, "start", 0), stop = (int64_t) pnum(t, "stop", 0);
    double ev = pnum(t, "eventId", 0);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "timerid", pnum(t, "timerId", 0));
    cJSON_AddStringToObject(o, "label", pstr(t, "title"));
    if (has(props, "title")) cJSON_AddStringToObject(o, "title", pstr(t, "title"));
    if (has(props, "summary"))
        cJSON_AddStringToObject(o, "summary", pstr(t, "subtitle")[0] ? pstr(t, "subtitle") : pstr(t, "description"));
    if (has(props, "channelid")) cJSON_AddNumberToObject(o, "channelid", pnum(t, "channelNumber", 0));
    if (has(props, "isradio")) cJSON_AddBoolToObject(o, "isradio", 0);
    if (has(props, "istimerrule")) cJSON_AddBoolToObject(o, "istimerrule", 0);
    if (has(props, "ismanual")) cJSON_AddBoolToObject(o, "ismanual", ev <= 0);
    if (has(props, "starttime")) add_time(o, "starttime", start);
    if (has(props, "endtime")) add_time(o, "endtime", stop);
    if (has(props, "state"))
        cJSON_AddStringToObject(o, "state", !strcmp(pstr(t, "state"), "recording") ? "recording" : "scheduled");
    if (has(props, "epguid")) cJSON_AddNumberToObject(o, "epguid", ev);
    if (has(props, "broadcastid")) cJSON_AddNumberToObject(o, "broadcastid", ev);
    if (has(props, "runtime")) cJSON_AddNumberToObject(o, "runtime", (double) ((stop - start) / 60));
    if (has(props, "directory")) cJSON_AddStringToObject(o, "directory", pstr(t, "seriesTitle"));
    return o;
}

static cJSON *get_timers(const cJSON *params, uint32_t only, rpc_err *e) {
    cJSON *all = tvrec_list(1);
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(params, "properties");
    cJSON *res = cJSON_CreateObject(), *arr = only ? NULL : cJSON_AddArrayToObject(res, "timers");
    const cJSON *t;
    int n = 0;
    cJSON_ArrayForEach(t, all) {
        if (only && (uint32_t) pnum(t, "timerId", 0) != only) continue;
        if (only) cJSON_AddItemToObject(res, "timerdetails", timer_item(t, props));
        else cJSON_AddItemToArray(arr, timer_item(t, props));
        n++;
    }
    cJSON_Delete(all);
    if (only && !n) {
        cJSON_Delete(res);
        return fail(e, -32602, "Timer not found");
    }
    if (!only) limits(res, 0, n, n);
    return res;
}

/* timer action in tvh; the reply to Kore comes with the result */
static void action_done(void *user, int ok, const char *err) {
    later *l = user;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", l->id ? l->id : cJSON_CreateNull());
    if (ok) cJSON_AddStringToObject(r, "result", "OK");
    else {
        cJSON *e = cJSON_AddObjectToObject(r, "error");
        cJSON_AddNumberToObject(e, "code", -32100);
        cJSON_AddStringToObject(e, "message", err && *err ? err : "failed");
    }
    reply(l->cid, l->tcp, cJSON_PrintUnformatted(r));
    cJSON_Delete(r);
    free(l);
}

static cJSON *timer_action(const char *action, uint32_t event, uint32_t timer, const char *autorec, int channel) {
    if (!cur_later) { /* in a batch: without a result */
        tvrec_action(action, event, timer, autorec, channel, NULL, NULL);
        return cJSON_CreateString("OK");
    }
    later *l = cur_later;
    cur_later = NULL;
    tvrec_action(action, event, timer, autorec, channel, action_done, l);
    return DEFER;
}

static cJSON *add_timer(const cJSON *params, rpc_err *e) {
    uint32_t ev = (uint32_t) pnum(params, "broadcastid", 0);
    int num = 0;
    tvsrc_event x;
    if (!tvsrc_event_by_id(ev, &x, &num)) return fail(e, -32602, "Broadcast not found");
    return timer_action(cJSON_IsTrue(cJSON_GetObjectItem(params, "timerrule")) ? "recordSeries" : "record", ev, 0, NULL,
                        num);
}

static cJSON *toggle_timer(const cJSON *params, rpc_err *e) {
    uint32_t ev = (uint32_t) pnum(params, "broadcastid", 0), tid = 0;
    char ar[48] = "";
    if (!tvrec_timer_of_event(ev, &tid, ar, sizeof(ar), NULL)) return add_timer(params, e);
    int num = 0;
    tvsrc_event x;
    tvsrc_event_by_id(ev, &x, &num);
    if (cJSON_IsTrue(cJSON_GetObjectItem(params, "timerrule")) && ar[0])
        return timer_action("cancelSeries", ev, 0, ar, num);
    return timer_action("cancel", ev, tid, NULL, num);
}

static cJSON *record(const cJSON *params, rpc_err *e) {
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(params, "channel");
    int num = cJSON_IsNumber(ch) ? (int) ch->valuedouble : act.channel();
    tvsrc_event now;
    if (!tvsrc_event_info(num, 0, &now)) return fail(e, -32602, "No current broadcast on this channel");
    uint32_t tid = 0;
    int has_timer = tvrec_timer_of_event(now.id, &tid, NULL, 0, NULL);
    const cJSON *rv = cJSON_GetObjectItemCaseSensitive(params, "record");
    int want = cJSON_IsBool(rv) ? cJSON_IsTrue(rv) : !has_timer; /* "toggle" or missing */
    if (want == has_timer) return cJSON_CreateString("OK");
    return timer_action(want ? "record" : "cancel", now.id, tid, NULL, num);
}

/* ---- Player ---- */

static int playing(uint32_t *rec, double *pos, double *dur, int *paused) {
    if (recplay_info(rec, pos, dur, paused)) return 2;
    if (act.channel() > 0) {
        if (paused) *paused = act.live_paused();
        return 1;
    }
    return 0;
}

static cJSON *active_players(void) {
    cJSON *a = cJSON_CreateArray();
    if (playing(NULL, NULL, NULL, NULL)) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "playerid", 1);
        cJSON_AddStringToObject(p, "playertype", "internal");
        cJSON_AddStringToObject(p, "type", "video");
        cJSON_AddItemToArray(a, p);
    }
    return a;
}

static cJSON *player_properties(const cJSON *params) {
    uint32_t rnum = 0;
    double pos = 0, dur = 0;
    int paused = 0;
    int what = playing(&rnum, &pos, &dur, &paused);
    if (what == 1) { /* live: position within the current event */
        tvsrc_event now;
        if (tvsrc_event_info(act.channel(), 0, &now) && now.stop > now.start) {
            pos = (double) (time(NULL) - now.start);
            dur = (double) (now.stop - now.start);
        }
    }
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(params, "properties"), *p;
    cJSON *r = cJSON_CreateObject();
    cJSON_ArrayForEach(p, props) {
        if (!cJSON_IsString(p)) continue;
        const char *k = p->valuestring;
        if (!strcmp(k, "speed")) cJSON_AddNumberToObject(r, k, what && !paused ? 1 : 0);
        else if (!strcmp(k, "live")) cJSON_AddBoolToObject(r, k, what == 1);
        else if (!strcmp(k, "type")) cJSON_AddStringToObject(r, k, what == 2 ? "video" : "channel");
        else if (!strcmp(k, "time")) cJSON_AddItemToObject(r, k, kodi_time(pos));
        else if (!strcmp(k, "totaltime")) cJSON_AddItemToObject(r, k, kodi_time(dur));
        else if (!strcmp(k, "percentage")) cJSON_AddNumberToObject(r, k, dur > 0 ? (pos / dur > 1 ? 100 : pos / dur * 100) : 0);
        else if (!strcmp(k, "playlistid") || !strcmp(k, "position")) cJSON_AddNumberToObject(r, k, -1);
        else if (!strcmp(k, "repeat")) cJSON_AddStringToObject(r, k, "off");
        else if (!strcmp(k, "canseek") || !strcmp(k, "canchangespeed")) cJSON_AddBoolToObject(r, k, what == 2);
        else if (!strcmp(k, "currentsubtitle")) cJSON_AddNullToObject(r, k);
        else if (!strcmp(k, "subtitles")) cJSON_AddItemToObject(r, k, cJSON_CreateArray());
        else if (!strcmp(k, "currentaudiostream") || !strcmp(k, "audiostreams")) {
            cJSON *s = cJSON_CreateObject();
            cJSON_AddNumberToObject(s, "index", 0);
            cJSON_AddStringToObject(s, "name", "Default");
            cJSON_AddStringToObject(s, "language", "");
            cJSON_AddStringToObject(s, "codec", "");
            if (k[0] == 'c') cJSON_AddItemToObject(r, k, s);
            else {
                cJSON *a = cJSON_AddArrayToObject(r, k);
                cJSON_AddItemToArray(a, s);
            }
        } else if (!strncmp(k, "can", 3) || !strcmp(k, "shuffled") || !strcmp(k, "partymode") ||
                   !strcmp(k, "subtitleenabled"))
            cJSON_AddBoolToObject(r, k, 0);
    }
    return r;
}

static cJSON *player_item(const cJSON *params) {
    const cJSON *props = cJSON_GetObjectItemCaseSensitive(params, "properties");
    uint32_t rnum = 0;
    cJSON *res = cJSON_CreateObject();
    if (playing(&rnum, NULL, NULL, NULL) == 2) {
        cJSON *r = tvrec_recording(rnum);
        if (r) {
            cJSON *it = recording_item(r, props);
            cJSON_AddStringToObject(it, "type", "recording");
            cJSON_AddNumberToObject(it, "id", rnum);
            cJSON_AddItemToObject(res, "item", it);
            cJSON_Delete(r);
            return res;
        }
    }
    int num = act.channel();
    char name[256] = "", icon[600] = "", img[900];
    tvsrc_channel_meta(num, name, sizeof(name), icon, sizeof(icon));
    image_url(icon, img, sizeof(img));
    tvsrc_event now;
    int have = tvsrc_event_info(num, 0, &now);
    cJSON *it = cJSON_AddObjectToObject(res, "item");
    cJSON_AddNumberToObject(it, "id", num);
    cJSON_AddStringToObject(it, "type", "channel");
    cJSON_AddStringToObject(it, "label", name);
    const char *plot = have ? (now.text[0] ? now.text : now.summary) : "";
    const cJSON *p;
    cJSON_ArrayForEach(p, props) {
        if (!cJSON_IsString(p)) continue;
        const char *k = p->valuestring;
        if (!strcmp(k, "title") || !strcmp(k, "showtitle")) cJSON_AddStringToObject(it, k, have ? now.title : name);
        else if (!strcmp(k, "channel")) cJSON_AddStringToObject(it, k, name);
        else if (!strcmp(k, "channelnumber")) cJSON_AddNumberToObject(it, k, num);
        else if (!strcmp(k, "channeltype")) cJSON_AddStringToObject(it, k, "tv");
        else if (!strcmp(k, "plot") || !strcmp(k, "description")) cJSON_AddStringToObject(it, k, plot);
        else if (!strcmp(k, "runtime"))
            cJSON_AddNumberToObject(it, k, have && now.stop > now.start ? (double) ((now.stop - now.start) / 60) : 0);
        else if (!strcmp(k, "starttime")) add_time(it, k, have ? now.start : 0);
        else if (!strcmp(k, "endtime")) add_time(it, k, have ? now.stop : 0);
        else if (!strcmp(k, "episodename")) cJSON_AddStringToObject(it, k, have ? now.subtitle : "");
        else if (!strcmp(k, "thumbnail")) cJSON_AddStringToObject(it, k, img);
        else if (!strcmp(k, "fanart") || !strcmp(k, "file") || !strcmp(k, "album"))
            cJSON_AddStringToObject(it, k, "");
        else if (!strcmp(k, "art")) {
            cJSON *a = cJSON_AddObjectToObject(it, "art");
            if (img[0]) cJSON_AddStringToObject(a, "channel.icon", img);
        } else if (!strcmp(k, "artist") || !strcmp(k, "studio") || !strcmp(k, "director") || !strcmp(k, "genre"))
            cJSON_AddItemToObject(it, k, cJSON_CreateArray());
        else if (!strcmp(k, "episode") || !strcmp(k, "season")) cJSON_AddNumberToObject(it, k, -1);
        else if (!strcmp(k, "track") || !strcmp(k, "year")) cJSON_AddNumberToObject(it, k, 0);
    }
    return res;
}

static void recplay_cmd(const char *action, double pos) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "action", action);
    cJSON_AddNumberToObject(m, "position", pos);
    recplay_control(m);
    cJSON_Delete(m);
}

static cJSON *speed_result(int speed) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "speed", speed);
    return r;
}

static cJSON *play_pause(const cJSON *params, int set_speed) {
    int paused = 0;
    int what = playing(NULL, NULL, NULL, &paused);
    const cJSON *pl = cJSON_GetObjectItemCaseSensitive(params, set_speed ? "speed" : "play");
    int play = set_speed ? !(cJSON_IsNumber(pl) && pl->valuedouble == 0) : cJSON_IsBool(pl) ? cJSON_IsTrue(pl) : paused;
    if (what == 2) recplay_cmd(play ? "play" : "pause", 0);
    else if (what == 1 && play == paused) act.key(KEY_PLAYPAUSE); /* timeshift */
    else if (!what) return speed_result(0);
    return speed_result(play ? 1 : 0);
}

static cJSON *seek(const cJSON *params, rpc_err *e) {
    double pos = 0, dur = 0;
    if (playing(NULL, &pos, &dur, NULL) != 2) return fail(e, -32602, "Seeking not possible in live TV");
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(params, "value");
    double target = pos;
    if (cJSON_IsNumber(v)) target = dur * v->valuedouble / 100;
    else if (cJSON_IsString(v) || cJSON_IsString(cJSON_GetObjectItem(v, "step"))) {
        const char *s = cJSON_IsString(v) ? v->valuestring : pstr(v, "step");
        target = pos + (!strcmp(s, "smallforward") ? 30 : !strcmp(s, "smallbackward") ? -10
                        : !strcmp(s, "bigforward")   ? 600
                        : !strcmp(s, "bigbackward")  ? -600 : 0);
    } else if (cJSON_GetObjectItem(v, "percentage")) target = dur * pnum(v, "percentage", 0) / 100;
    else if (cJSON_GetObjectItem(v, "time")) {
        const cJSON *t = cJSON_GetObjectItem(v, "time");
        target = pnum(t, "hours", 0) * 3600 + pnum(t, "minutes", 0) * 60 + pnum(t, "seconds", 0);
    } else if (cJSON_GetObjectItem(v, "seconds")) target = pos + pnum(v, "seconds", 0);
    if (target > dur - 5 && dur > 5) target = dur - 5;
    if (target < 0) target = 0;
    recplay_cmd("seek", target);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "percentage", dur > 0 ? target / dur * 100 : 0);
    cJSON_AddItemToObject(r, "time", kodi_time(target));
    cJSON_AddItemToObject(r, "totaltime", kodi_time(dur));
    return r;
}

static cJSON *player_open(const cJSON *params, rpc_err *e) {
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(params, "item");
    if (cJSON_GetObjectItem(it, "channelid")) {
        act.front();
        act.tune((int) pnum(it, "channelid", 0));
        return cJSON_CreateString("OK");
    }
    if (cJSON_GetObjectItem(it, "recordingid")) {
        if (!tvrec_play((uint32_t) pnum(it, "recordingid", 0), 0)) return fail(e, -32602, "Recording not found");
        act.front();
        return cJSON_CreateString("OK");
    }
    return fail(e, -32602, "Invalid item for Player.Open");
}

/* ---- Keys ---- */

static cJSON *input_action(const char *a) {
    static const struct {
        const char *name;
        int key;
    } map[] = {{"up", KEY_UP},           {"down", KEY_DOWN},       {"left", KEY_LEFT},
               {"right", KEY_RIGHT},     {"select", KEY_OK},       {"back", KEY_BACK},
               {"info", KEY_INFO},       {"osd", KEY_INFO},        {"playpause", KEY_PLAYPAUSE},
               {"pause", KEY_PAUSE},     {"play", KEY_PLAY},       {"stop", KEY_STOP},
               {"fastforward", KEY_FFWD}, {"rewind", KEY_REWIND},  {"record", KEY_RECORD},
               {"red", KEY_RED},         {"green", KEY_GREEN},     {"yellow", KEY_YELLOW},
               {"blue", KEY_BLUE},       {"previousmenu", KEY_BACK}, {"close", KEY_BACK}};
    if (!strcmp(a, "channelup") || !strcmp(a, "channeldown")) {
        act.front();
        act.step(a[7] == 'u' ? 1 : -1);
    }
    else if (!strncmp(a, "number", 6) && a[6] >= '0' && a[6] <= '9' && !a[7]) act.key(KEY_DIGIT0 + (a[6] - '0'));
    else
        for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (!strcmp(a, map[i].name)) act.key(map[i].key);
    return cJSON_CreateString("OK");
}

/* ---- Everything else ---- */

static cJSON *props_obj(const cJSON *params, const char *const *kv, int n) {
    cJSON *r = cJSON_CreateObject();
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(params, "properties")) {
        if (!cJSON_IsString(p)) continue;
        for (int i = 0; i < n; i++)
            if (!strcmp(p->valuestring, kv[2 * i])) {
                const char *v = kv[2 * i + 1];
                if (!strcmp(v, "true") || !strcmp(v, "false")) cJSON_AddBoolToObject(r, p->valuestring, v[0] == 't');
                else if (v[0] == '{') cJSON_AddItemToObject(r, p->valuestring, cJSON_Parse(v));
                else if (v[0] >= '0' && v[0] <= '9') cJSON_AddNumberToObject(r, p->valuestring, atof(v));
                else cJSON_AddStringToObject(r, p->valuestring, v);
            }
    }
    return r;
}

static cJSON *empty_list(const char *key) {
    cJSON *r = cJSON_CreateObject();
    cJSON_AddItemToObject(r, key, cJSON_CreateArray());
    limits(r, 0, 0, 0);
    return r;
}

static cJSON *version(void) {
    cJSON *r = cJSON_CreateObject(), *v = cJSON_AddObjectToObject(r, "version");
    cJSON_AddNumberToObject(v, "major", KODI_MAJOR);
    cJSON_AddNumberToObject(v, "minor", 0);
    cJSON_AddNumberToObject(v, "patch", 0);
    return r;
}

static cJSON *introspect(void) {
    static const char *const methods[] = {
        "JSONRPC.Ping", "JSONRPC.Version", "JSONRPC.GetConfiguration", "JSONRPC.Introspect", "JSONRPC.Permission",
        "Application.GetProperties", "Player.GetActivePlayers", "Player.GetProperties", "Player.GetItem",
        "Player.Open", "Player.GoTo", "Player.Stop", "Player.PlayPause", "Player.SetSpeed", "Player.Seek",
        "PVR.GetChannelGroups", "PVR.GetChannels", "PVR.GetChannelDetails", "PVR.GetBroadcasts",
        "PVR.GetBroadcastDetails", "PVR.GetRecordings", "PVR.GetRecordingDetails", "PVR.GetTimers",
        "PVR.GetTimerDetails", "PVR.AddTimer", "PVR.DeleteTimer", "PVR.ToggleTimer", "PVR.Record",
        "Input.ExecuteAction", "Input.Up", "Input.Down", "Input.Left", "Input.Right", "Input.Select", "Input.Back",
        "Input.Info", "Input.ShowOSD", "GUI.GetProperties"};
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "id", "http://xbmc.org/jsonrpc/ServiceDescription.json");
    cJSON_AddStringToObject(r, "description", "TVF Kodi-compatible JSON-RPC API");
    cJSON *v = version();
    cJSON_AddItemToObject(r, "version", cJSON_DetachItemFromObject(v, "version"));
    cJSON_Delete(v);
    cJSON *m = cJSON_AddObjectToObject(r, "methods");
    for (size_t i = 0; i < sizeof(methods) / sizeof(methods[0]); i++) {
        cJSON *d = cJSON_AddObjectToObject(m, methods[i]);
        cJSON_AddItemToObject(d, "params", cJSON_CreateArray());
    }
    cJSON *n = cJSON_AddObjectToObject(r, "notifications");
    const char *nt[] = {"Player.OnPlay", "Player.OnPause", "Player.OnResume", "Player.OnStop"};
    for (int i = 0; i < 4; i++) cJSON_AddStringToObject(cJSON_AddObjectToObject(n, nt[i]), "type", "notification");
    cJSON_AddObjectToObject(r, "types");
    return r;
}

static cJSON *dispatch(const char *m, const cJSON *p, rpc_err *e) {
    static const char *const app_props[] = {"volume", "100", "muted", "false", "name", "TVF",
                                            "version", "{\"major\":12,\"minor\":0,\"patch\":0}"};
    static const char *const gui_props[] = {"currentwindow", "{\"id\":10601,\"label\":\"PVR TV Channels\"}",
                                            "currentcontrol", "{\"label\":\"\"}",
                                            "skin", "{\"id\":\"skin.estuary\",\"name\":\"Estuary\"}",
                                            "fullscreen", "true",
                                            "stereoscopicmode", "{\"mode\":\"off\",\"label\":\"Off\"}"};
    static const char *const sys_props[] = {"canshutdown", "false", "canhibernate", "false",
                                            "canreboot", "false", "cansuspend", "false"};
    if (!strcmp(m, "JSONRPC.Ping")) return cJSON_CreateString("pong");
    if (!strcmp(m, "JSONRPC.Version")) return version();
    if (!strcmp(m, "JSONRPC.GetConfiguration"))
        return cJSON_Parse("{\"notifications\":{\"gui\":true,\"system\":true,\"player\":true,\"other\":true}}");
    if (!strcmp(m, "JSONRPC.Introspect")) return introspect();
    if (!strcmp(m, "JSONRPC.Permission"))
        return cJSON_Parse("{\"ControlPlayback\":true,\"ControlPower\":false,\"ControlGUI\":true,\"ControlPVR\":true,"
                           "\"Navigate\":true,\"ReadData\":true,\"WriteFile\":false,\"ControlNotify\":true,"
                           "\"ControlSystem\":false,\"ExecuteAddon\":false,\"ManageAddon\":false,"
                           "\"UpdateData\":false,\"RemoveData\":false}");
    if (!strcmp(m, "Application.GetProperties")) {
        cJSON *r = props_obj(p, app_props, 4);
        if (cJSON_GetObjectItem(r, "name")) cJSON_ReplaceItemInObject(r, "name", cJSON_CreateString(cfg.name));
        return r;
    }
    if (!strcmp(m, "Application.SetVolume") || !strcmp(m, "Application.SetMute")) return cJSON_CreateNumber(100);
    if (!strcmp(m, "XBMC.GetInfoLabels")) { /* this is how Kore gets the MAC for Wake-on-LAN */
        cJSON *r = cJSON_CreateObject();
        const cJSON *l;
        cJSON_ArrayForEach(l, cJSON_GetObjectItemCaseSensitive(p, "labels")) {
            if (!cJSON_IsString(l)) continue;
            const char *k = l->valuestring;
            cJSON_AddStringToObject(r, k, !strcasecmp(k, "Network.MacAddress")       ? own_mac
                                          : !strcasecmp(k, "Network.IPAddress")      ? own_ip
                                          : !strcasecmp(k, "System.FriendlyName")    ? cfg.name
                                          : !strcasecmp(k, "System.BuildVersion")    ? "TVF"
                                                                                       : "");
        }
        return r;
    }
    if (!strcmp(m, "XBMC.GetInfoBooleans")) {
        cJSON *r = cJSON_CreateObject();
        const cJSON *l;
        cJSON_ArrayForEach(l, cJSON_GetObjectItemCaseSensitive(p, "booleans"))
            if (cJSON_IsString(l)) cJSON_AddBoolToObject(r, l->valuestring, 0);
        return r;
    }
    if (!strcmp(m, "Player.GetActivePlayers")) return active_players();
    if (!strcmp(m, "Player.GetProperties")) return player_properties(p);
    if (!strcmp(m, "Player.GetItem")) return player_item(p);
    if (!strcmp(m, "Player.Open")) return player_open(p, e);
    if (!strcmp(m, "Player.GoTo")) {
        const char *to = pstr(p, "to");
        if (!strcmp(to, "next") || !strcmp(to, "previous")) {
            act.front();
            act.step(to[0] == 'n' ? 1 : -1);
        }
        return cJSON_CreateString("OK");
    }
    if (!strcmp(m, "Player.Stop")) {
        act.stop();
        return cJSON_CreateString("OK");
    }
    if (!strcmp(m, "Player.PlayPause")) return play_pause(p, 0);
    if (!strcmp(m, "Player.SetSpeed")) return play_pause(p, 1);
    if (!strcmp(m, "Player.Seek")) return seek(p, e);
    if (!strcmp(m, "Playlist.GetItems")) return empty_list("items");
    if (!strcmp(m, "Playlist.GetPlaylists"))
        return cJSON_Parse("[{\"playlistid\":0,\"type\":\"audio\"},{\"playlistid\":1,\"type\":\"video\"},"
                           "{\"playlistid\":2,\"type\":\"picture\"}]");
    if (!strcmp(m, "PVR.GetChannelGroups")) return get_channel_groups(p);
    if (!strcmp(m, "PVR.GetChannels")) return get_channels(p);
    if (!strcmp(m, "PVR.GetChannelDetails")) return get_channel_details(p, e);
    if (!strcmp(m, "PVR.GetBroadcasts")) return get_broadcasts(p);
    if (!strcmp(m, "PVR.GetBroadcastDetails")) return get_broadcast_details(p, e);
    if (!strcmp(m, "PVR.GetRecordings")) return get_recordings(p);
    if (!strcmp(m, "PVR.GetRecordingDetails")) return get_recording_details(p, e);
    if (!strcmp(m, "PVR.GetTimers")) return get_timers(p, 0, e);
    if (!strcmp(m, "PVR.GetTimerDetails")) {
        uint32_t id = (uint32_t) pnum(p, "timerid", 0);
        return id ? get_timers(p, id, e) : fail(e, -32602, "Timer not found");
    }
    if (!strcmp(m, "PVR.AddTimer")) return add_timer(p, e);
    if (!strcmp(m, "PVR.DeleteTimer")) return timer_action("cancel", 0, (uint32_t) pnum(p, "timerid", 0), NULL, 0);
    if (!strcmp(m, "PVR.ToggleTimer")) return toggle_timer(p, e);
    if (!strcmp(m, "PVR.Record")) return record(p, e);
    if (!strcmp(m, "Input.ExecuteAction")) return input_action(pstr(p, "action"));
    if (!strncmp(m, "Input.", 6)) {
        static const char *const in[][2] = {{"Up", "up"},         {"Down", "down"},     {"Left", "left"},
                                            {"Right", "right"},   {"Select", "select"}, {"Back", "back"},
                                            {"Info", "info"},     {"ShowOSD", "osd"},   {"ContextMenu", "info"},
                                            {"Home", "back"}};
        for (size_t i = 0; i < sizeof(in) / sizeof(in[0]); i++)
            if (!strcmp(m + 6, in[i][0])) return input_action(in[i][1]);
        return cJSON_CreateString("OK");
    }
    if (!strcmp(m, "GUI.GetProperties")) return props_obj(p, gui_props, 5);
    if (!strcmp(m, "GUI.ActivateWindow") || !strcmp(m, "GUI.ShowNotification")) return cJSON_CreateString("OK");
    if (!strcmp(m, "System.GetProperties")) return props_obj(p, sys_props, 4);
    if (!strcmp(m, "Addons.GetAddons")) return empty_list("addons");
    if (!strcmp(m, "Files.GetSources")) return empty_list("sources");
    if (!strcmp(m, "Favourites.GetFavourites")) return empty_list("favourites");
    if (!strcmp(m, "Profiles.GetCurrentProfile")) return cJSON_Parse("{\"label\":\"Default\"}");
    if (!strcmp(m, "Profiles.GetProfiles")) {
        cJSON *r = cJSON_Parse("{\"profiles\":[{\"label\":\"Default\"}]}");
        limits(r, 0, 1, 1);
        return r;
    }
    if (!strcmp(m, "Files.PrepareDownload")) {
        char path[2048];
        snprintf(path, sizeof(path), "%s", pstr(p, "path"));
        char *u = path;
        if (!strncmp(u, "image://", 8)) {
            u += 8;
            size_t l = strlen(u);
            if (l && u[l - 1] == '/') u[l - 1] = 0;
        } else {
            char enc[2048];
            image_url(u, enc, sizeof(enc)); /* /image/ also accepts the image:// wrapper */
            snprintf(path, sizeof(path), "%s", enc);
            u = path;
        }
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "mode", "redirect");
        cJSON_AddStringToObject(r, "protocol", "http");
        char full[2200];
        snprintf(full, sizeof(full), "/image/%s", u);
        cJSON_AddStringToObject(cJSON_AddObjectToObject(r, "details"), "path", full);
        return r;
    }
    return fail(e, -32601, "Method not found");
}

/* one request -> reply (NULL for a notification without id, DEFER) */
/* allowed via GET: queries only */
static int read_only(const char *m) {
    const char *d = strchr(m, '.');
    return (d && !strncmp(d + 1, "Get", 3)) || !strcmp(m, "JSONRPC.Ping") || !strcmp(m, "JSONRPC.Version") ||
           !strcmp(m, "JSONRPC.Introspect");
}

static cJSON *handle_one(const cJSON *req, uint32_t cid, int tcp, int ro, int batch) {
    const char *method = pstr(req, "method");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(req, "id");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(req, "params");
    if (strcmp(method, "JSONRPC.Ping") && strncmp(method, "Player.GetProperties", 20) &&
        strcmp(method, "Player.GetActivePlayers"))
        mt_log("[kodi] %s", method);
    rpc_err e = {0, ""};
    later *l = NULL;
    if (!batch && id) {
        l = calloc(1, sizeof(*l));
        if (l) {
            l->cid = cid;
            l->tcp = tcp;
            l->id = cJSON_Duplicate(id, 1);
        }
    }
    cJSON *res;
    if (ro && !read_only(method)) {
        res = fail(&e, -32600, "only queries via GET, use POST");
    } else {
        cur_later = l;
        res = dispatch(method, params, &e);
        if (res == DEFER) return DEFER; /* l now belongs to the action */
        cur_later = NULL;
    }
    if (l) {
        cJSON_Delete(l->id);
        free(l);
    }
    if (!id) {
        cJSON_Delete(res);
        return NULL;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "jsonrpc", "2.0");
    cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
    if (res) cJSON_AddItemToObject(r, "result", res);
    else {
        cJSON *er = cJSON_AddObjectToObject(r, "error");
        cJSON_AddNumberToObject(er, "code", e.code ? e.code : -32603);
        cJSON_AddStringToObject(er, "message", e.msg);
        mt_log("[kodi] %s: %s", method, e.msg);
    }
    return r;
}

/* ---- Notifications ---- */

static void notify(const char *method, cJSON *data) {
    cJSON *n = cJSON_CreateObject();
    cJSON_AddStringToObject(n, "jsonrpc", "2.0");
    cJSON_AddStringToObject(n, "method", method);
    cJSON *p = cJSON_AddObjectToObject(n, "params");
    cJSON_AddItemToObject(p, "data", data);
    cJSON_AddStringToObject(p, "sender", "xbmc");
    reply(0, 1, cJSON_PrintUnformatted(n));
    cJSON_Delete(n);
}

static cJSON *item_ref(int what, int ch, uint32_t rec) {
    cJSON *it = cJSON_CreateObject();
    if (what == 2) {
        cJSON_AddStringToObject(it, "type", "recording");
        cJSON_AddNumberToObject(it, "id", rec);
    } else {
        cJSON_AddStringToObject(it, "type", "channel");
        cJSON_AddNumberToObject(it, "id", ch);
        cJSON_AddStringToObject(it, "channeltype", "tv");
    }
    return it;
}

static cJSON *with_player(cJSON *item, int speed) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "item", item);
    cJSON *pl = cJSON_AddObjectToObject(d, "player");
    cJSON_AddNumberToObject(pl, "playerid", 1);
    cJSON_AddNumberToObject(pl, "speed", speed);
    return d;
}

static void watch_state(void) {
    static struct {
        int what, ch, paused;
        uint32_t rec;
    } last;
    static uint32_t at;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t now = (uint32_t) (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
    if (now - at < STATE_EVERY_MS) return;
    at = now;
    uint32_t rec = 0;
    int paused = 0;
    int what = playing(&rec, NULL, NULL, &paused), ch = what == 1 ? act.channel() : 0;
    int send = __atomic_load_n(&n_tcp, __ATOMIC_RELAXED) > 0;
    if (what && (what != last.what || ch != last.ch || rec != last.rec)) {
        if (send) notify("Player.OnPlay", with_player(item_ref(what, ch, rec), paused ? 0 : 1));
    } else if (what && paused != last.paused) {
        if (send) notify(paused ? "Player.OnPause" : "Player.OnResume", with_player(item_ref(what, ch, rec), paused ? 0 : 1));
    } else if (!what && last.what) {
        cJSON *d = cJSON_CreateObject();
        cJSON *it = cJSON_AddObjectToObject(d, "item");
        cJSON_AddStringToObject(it, "type", last.what == 2 ? "recording" : "channel");
        cJSON_AddBoolToObject(d, "end", 0);
        if (send) notify("Player.OnStop", d);
        else cJSON_Delete(d);
    }
    last.what = what;
    last.ch = ch;
    last.rec = rec;
    last.paused = paused;
}

void kodi_poll(void) {
    if (wake_pipe[1] < 0) return;
    msg *m;
    int any = 0;
    while ((m = pop(&jobs, &jobs_tail)) != NULL) {
        __atomic_sub_fetch(&n_jobs, 1, __ATOMIC_RELAXED);
        any = 1;
        cJSON *req = cJSON_Parse(m->text);
        cJSON *out = NULL;
        if (!req) {
            out = cJSON_Parse("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}");
        } else if (cJSON_IsArray(req)) {
            out = cJSON_CreateArray();
            const cJSON *r;
            cJSON_ArrayForEach(r, req) {
                cJSON *one = handle_one(r, m->cid, m->tcp, m->ro, 1);
                if (one) cJSON_AddItemToArray(out, one);
            }
        } else {
            out = handle_one(req, m->cid, m->tcp, m->ro, 0);
        }
        cJSON_Delete(req);
        if (out == DEFER) {
        } else if (out) {
            reply(m->cid, m->tcp, cJSON_PrintUnformatted(out));
            cJSON_Delete(out);
        } else if (!m->tcp) {
            reply(m->cid, 0, strdup("")); /* HTTP needs a reply */
        }
        free(m->text);
        free(m);
    }
    if (any && act.request) act.request();
    trusted_save();
    watch_state();
}
