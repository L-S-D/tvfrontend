/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE /* strcasestr */
#include "http.h"

#include "md5.h"
#include "log.h"

#include <errno.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <pthread.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

typedef struct {
    int tls;
    char host[128];
    char port[8];
    char path[4096];
    char user[64];
    char pass[64];
} url_parts;

/* %XX in the URL's user/password (http_url_userinfo encodes this way) */
static void pct_decode(char *s) {
    char *o = s;
    for (char *i = s; *i; i++) {
        int h1, h2;
        if (i[0] == '%' && i[1] && i[2] && sscanf(i + 1, "%1x%1x", (unsigned *) &h1, (unsigned *) &h2) == 2) {
            *o++ = (char) (h1 * 16 + h2);
            i += 2;
        } else {
            *o++ = *i;
        }
    }
    *o = 0;
}

static int parse_url(const char *url, url_parts *u) {
    memset(u, 0, sizeof(*u));
    const char *p;
    if (strncmp(url, "http://", 7) == 0) p = url + 7;
    else if (strncmp(url, "https://", 8) == 0) {
        p = url + 8;
        u->tls = 1;
    } else {
        return -1;
    }
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *at = memchr(p, '@', (size_t) (hostend - p));
    if (at) {
        const char *colon = memchr(p, ':', (size_t) (at - p));
        size_t ul = (size_t) ((colon ? colon : at) - p);
        if (ul >= sizeof(u->user)) {
            return -1;
        }
        memcpy(u->user, p, ul);
        if (colon) {
            size_t pl = (size_t) (at - colon - 1);
            if (pl >= sizeof(u->pass)) {
                return -1;
            }
            memcpy(u->pass, colon + 1, pl);
        }
        pct_decode(u->user);
        pct_decode(u->pass);
        p = at + 1;
    }
    const char *colon = memchr(p, ':', (size_t) (hostend - p));
    size_t hl = (size_t) ((colon ? colon : hostend) - p);
    if (hl == 0 || hl >= sizeof(u->host)) {
        return -1;
    }
    memcpy(u->host, p, hl);
    if (colon) {
        size_t pl = (size_t) (hostend - colon - 1);
        if (pl == 0 || pl >= sizeof(u->port)) {
            return -1;
        }
        memcpy(u->port, colon + 1, pl);
    } else {
        strcpy(u->port, u->tls ? "443" : "80");
    }
    snprintf(u->path, sizeof(u->path), "%s", slash ? slash : "/");
    return 0;
}

/* register the socket with the owner (-1 = unregister); 1 = request cancelled */
static int hook_fd(const http_hook *h, int fd) { return h && h->fd ? h->fd(h->user, fd) : 0; }

/* unregister and close the socket; 1 = the request was cancelled */
static int drop_fd(const http_hook *h, int fd) {
    int cancelled = hook_fd(h, -1);
    close(fd);
    return cancelled;
}

static int tcp_connect(const url_parts *u, const http_hook *h) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(u->host, u->port, &hints, &res) != 0 || !res) {
        mt_log("[http] cannot resolve name: %s", u->host);
        return -1;
    }
    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }
    /* register right away: from here on the owner can cancel connect() and recv() */
    if (hook_fd(h, fd)) {
        drop_fd(h, fd);
        freeaddrinfo(res);
        return -1;
    }
    /* Large receive buffer: a start burst from the TVH buffer (several
     * seconds of TS) should arrive without TCP congestion. */
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    struct timeval tv = {.tv_sec = 8, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        int e = errno;
        if (!drop_fd(h, fd)) mt_log("[http] connect %s:%s: %s", u->host, u->port, strerror(e));
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    /* cancelled before connect() ran: shutdown() on the not yet
     * connected socket does not stop connect() */
    if (hook_fd(h, fd)) {
        drop_fd(h, fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/* ---------------- TLS (OpenSSL, static) ---------------- */

static char g_ca_file[512] = "/etc/ssl/certs/ca-certificates.crt";
static SSL_CTX *g_ctx;
static pthread_once_t g_ctx_once = PTHREAD_ONCE_INIT;

void http_set_ca_file(const char *path) { snprintf(g_ca_file, sizeof(g_ca_file), "%s", path ? path : ""); }

static void ctx_init(void) {
    g_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_ctx) return;
    SSL_CTX_set_min_proto_version(g_ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(g_ctx, SSL_VERIFY_PEER, NULL);
    if (SSL_CTX_load_verify_locations(g_ctx, g_ca_file, NULL) != 1)
        mt_log("[http] cannot load root certificates %s", g_ca_file);
}

/* TLS over the connected socket; 0 = ok */
static int tls_start(http_conn *c, const url_parts *u) {
    pthread_once(&g_ctx_once, ctx_init);
    if (!g_ctx) return -1;
    SSL *ssl = SSL_new(g_ctx);
    if (!ssl) return -1;
    SSL_set_fd(ssl, c->fd);
    SSL_set_tlsext_host_name(ssl, u->host);
    SSL_set_hostflags(ssl, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
    SSL_set1_host(ssl, u->host);
    if (SSL_connect(ssl) != 1) {
        long vr = SSL_get_verify_result(ssl);
        unsigned long e = ERR_get_error();
        char eb[160] = "";
        if (e) ERR_error_string_n(e, eb, sizeof(eb));
        mt_log("[http] TLS %s: %s%s%s", u->host, vr != X509_V_OK ? X509_verify_cert_error_string(vr) : "Handshake",
               eb[0] ? " - " : "", eb);
        ERR_clear_error();
        SSL_free(ssl);
        return -1;
    }
    c->ssl = ssl;
    return 0;
}

static ssize_t io_send(http_conn *c, const void *b, size_t n) {
    if (c->ssl) {
        int r = SSL_write((SSL *) c->ssl, b, (int) n);
        return r > 0 ? r : -1;
    }
    return send(c->fd, b, n, MSG_NOSIGNAL);
}

static ssize_t io_recv(http_conn *c, void *b, size_t n) {
    if (c->ssl) {
        int r = SSL_read((SSL *) c->ssl, b, (int) n);
        if (r > 0) return r;
        int e = SSL_get_error((SSL *) c->ssl, r);
        ERR_clear_error();
        return e == SSL_ERROR_ZERO_RETURN ? 0 : -1;
    }
    ssize_t r;
    do {
        r = recv(c->fd, b, n, 0);
    } while (r < 0 && errno == EINTR);
    return r;
}

/* ---------------- Egress via proxies (countries) ---------------- */

static const char *(*g_proxy_for)(const char *host, char *buf, size_t n);

void http_set_proxy_resolver(const char *(*fn)(const char *host, char *buf, size_t n)) { g_proxy_for = fn; }

/* read a proxy's response header byte by byte (the tunnel starts after it) */
static int read_proxy_reply(int fd) {
    char rb[1024];
    size_t rl = 0;
    while (rl + 1 < sizeof(rb)) {
        ssize_t r = recv(fd, rb + rl, 1, 0);
        if (r <= 0) return -1;
        rl++;
        if (rl >= 4 && !memcmp(rb + rl - 4, "\r\n\r\n", 4)) break;
    }
    rb[rl] = 0;
    int st = 0;
    sscanf(rb, "HTTP/%*d.%*d %d", &st);
    return st;
}

/* Connection to host:port according to the egress rules. tunnel: through a proxy
 * via CONNECT (otherwise direct); without tunnel: to the proxy itself (*via_proxy = 1,
 * the request then carries the full URL) or directly to the target */
static int upstream(const url_parts *u, int tunnel, const http_hook *h, int *via_proxy) {
    char pb[256] = "";
    const char *px = g_proxy_for ? g_proxy_for(u->host, pb, sizeof(pb)) : NULL;
    *via_proxy = 0;
    if (!px || !*px) return tcp_connect(u, h);
    url_parts p;
    if (parse_url(px, &p) != 0) {
        mt_log("[http] invalid proxy: %s", px);
        return tcp_connect(u, h);
    }
    int fd = tcp_connect(&p, h);
    if (fd < 0) return -1;
    if (!tunnel) {
        *via_proxy = 1;
        return fd;
    }
    char req[512];
    int n = snprintf(req, sizeof(req), "CONNECT %s:%s HTTP/1.1\r\nHost: %s:%s\r\n\r\n", u->host, u->port, u->host,
                     u->port);
    if (n <= 0 || send(fd, req, (size_t) n, MSG_NOSIGNAL) != n) {
        drop_fd(h, fd);
        return -1;
    }
    int st = read_proxy_reply(fd);
    if (st != 200) {
        if (!drop_fd(h, fd)) mt_log("[http] Proxy %s: CONNECT %s:%s -> %d", px, u->host, u->port, st);
        return -1;
    }
    return fd;
}

int http_upstream(const char *host, const char *port, int tunnel, int *via_proxy) {
    url_parts u;
    memset(&u, 0, sizeof(u));
    snprintf(u.host, sizeof(u.host), "%s", host);
    snprintf(u.port, sizeof(u.port), "%s", port);
    return upstream(&u, tunnel, NULL, via_proxy);
}

/* unregister and close the connection (with TLS); 1 = the request was cancelled */
static int conn_drop(const http_hook *h, http_conn *c) {
    if (c->ssl) SSL_free((SSL *) c->ssl);
    c->ssl = NULL;
    int r = drop_fd(h, c->fd);
    c->fd = -1;
    return r;
}

static void b64(const char *in, char *out, size_t outlen) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t n = strlen(in), o = 0;
    for (size_t i = 0; i < n && o + 5 < outlen; i += 3) {
        uint32_t v = (uint32_t) (unsigned char) in[i] << 16;
        if (i + 1 < n) v |= (uint32_t) (unsigned char) in[i + 1] << 8;
        if (i + 2 < n) v |= (uint32_t) (unsigned char) in[i + 2];
        out[o++] = t[(v >> 18) & 63];
        out[o++] = t[(v >> 12) & 63];
        out[o++] = (i + 1 < n) ? t[(v >> 6) & 63] : '=';
        out[o++] = (i + 2 < n) ? t[v & 63] : '=';
    }
    out[o] = 0;
}

/* Value of a parameter from a WWW-Authenticate line, with/without quotes. */
static int auth_param(const char *hdr, const char *key, char *out, size_t outlen) {
    size_t kl = strlen(key);
    const char *p = hdr;
    while ((p = strcasestr(p, key)) != NULL) {
        if ((p == hdr || p[-1] == ' ' || p[-1] == ',' || p[-1] == '\t') && p[kl] == '=') {
            p += kl + 1;
            size_t i = 0;
            if (*p == '"') {
                p++;
                while (*p && *p != '"' && i + 1 < outlen) out[i++] = *p++;
            } else {
                while (*p && *p != ',' && *p != ' ' && *p != '\r' && i + 1 < outlen) out[i++] = *p++;
            }
            out[i] = 0;
            return 1;
        }
        p += kl;
    }
    return 0;
}

static void build_digest(const char *www, const url_parts *u, char *out, size_t outlen) {
    char realm[128] = "", nonce[256] = "", opaque[256] = "", qop[64] = "", algo[32] = "";
    auth_param(www, "realm", realm, sizeof(realm));
    auth_param(www, "nonce", nonce, sizeof(nonce));
    auth_param(www, "opaque", opaque, sizeof(opaque));
    auth_param(www, "qop", qop, sizeof(qop));
    auth_param(www, "algorithm", algo, sizeof(algo));

    char a1[512], a2[4200], ha1[33], ha2[33], resp[33], tmp[1600];
    snprintf(a1, sizeof(a1), "%s:%s:%s", u->user, realm, u->pass);
    md5_hex(a1, ha1);
    snprintf(a2, sizeof(a2), "GET:%s", u->path);
    md5_hex(a2, ha2);

    char cnonce[17];
    snprintf(cnonce, sizeof(cnonce), "%08lx%08lx", (unsigned long) mt_now_ns(), (unsigned long) getpid());
    int use_qop = strstr(qop, "auth") != NULL;
    if (use_qop) {
        snprintf(tmp, sizeof(tmp), "%s:%s:00000001:%s:auth:%s", ha1, nonce, cnonce, ha2);
    } else {
        snprintf(tmp, sizeof(tmp), "%s:%s:%s", ha1, nonce, ha2);
    }
    md5_hex(tmp, resp);

    int n = snprintf(out, outlen,
                     "Authorization: Digest username=\"%s\", realm=\"%s\", nonce=\"%s\", uri=\"%s\", "
                     "algorithm=MD5, response=\"%s\"",
                     u->user, realm, nonce, u->path, resp);
    if (use_qop && n > 0 && (size_t) n < outlen) {
        n += snprintf(out + n, outlen - (size_t) n, ", qop=auth, nc=00000001, cnonce=\"%s\"", cnonce);
    }
    if (opaque[0] && n > 0 && (size_t) n < outlen) {
        n += snprintf(out + n, outlen - (size_t) n, ", opaque=\"%s\"", opaque);
    }
    if (n > 0 && (size_t) n < outlen) {
        snprintf(out + n, outlen - (size_t) n, "\r\n");
    }
}

/* One request; reads the header, puts the start of the body into c->pre. www receives
 * WWW-Authenticate (if present). */
static __thread const char *t_extra; /* extra header lines (http_open_hdr) */
/* http_request: method, body, every status counts as a response, response header */
static __thread const char *t_method;
static __thread const void *t_body;
static __thread size_t t_body_len;
static __thread int t_any;
static __thread long long t_range_to = -1; /* http_open_span: end of the range */
static __thread char t_head[4096];

const char *http_last_head(void) { return t_head; }

/* send everything (body: send() may write partially) */
static int io_send_all(http_conn *c, const void *b, size_t n) {
    const char *p = b;
    while (n) {
        ssize_t w = io_send(c, p, n);
        if (w <= 0) return -1;
        p += w;
        n -= (size_t) w;
    }
    return 0;
}

static int do_request(const url_parts *u, const char *auth_line, long long range_from, const http_hook *h,
                      http_conn *c, char *www, size_t wwwlen) {
    int via_proxy = 0;
    /* https always as a tunnel, http through the proxy with the full URL */
    c->fd = upstream(u, u->tls, h, &via_proxy);
    if (c->fd < 0) {
        return -1;
    }
    if (u->tls && tls_start(c, u) != 0) {
        conn_drop(h, c);
        return -1;
    }
    char range[64] = "";
    if (range_from >= 0 && t_range_to >= range_from)
        snprintf(range, sizeof(range), "Range: bytes=%lld-%lld\r\n", range_from, t_range_to);
    else if (range_from >= 0) snprintf(range, sizeof(range), "Range: bytes=%lld-\r\n", range_from);
    char req[6144];
    /* Host without the default port (some CDNs compare exactly) */
    int defport = !strcmp(u->port, u->tls ? "443" : "80");
    char target[4400];
    if (via_proxy) snprintf(target, sizeof(target), "http://%s:%s%s", u->host, u->port, u->path);
    else snprintf(target, sizeof(target), "%s", u->path);
    const char *ex = t_extra ? t_extra : "";
    int ua = !strcasestr(ex, "User-Agent:");
    char clen[48] = "";
    if (t_body) snprintf(clen, sizeof(clen), "Content-Length: %zu\r\n", t_body_len);
    int n = snprintf(req, sizeof(req),
                     "%s %s HTTP/1.0\r\nHost: %s%s%s\r\n%sConnection: close\r\n%s%s%s%s\r\n",
                     t_method ? t_method : "GET", target, u->host, defport ? "" : ":", defport ? "" : u->port,
                     ua ? "User-Agent: TVF\r\n" : "", range, auth_line ? auth_line : "", ex, clen);
    /* MSG_NOSIGNAL: after a cancel (shutdown) there would otherwise be SIGPIPE */
    if (n <= 0 || (size_t) n >= sizeof(req) || io_send(c, req, (size_t) n) != n ||
        (t_body && t_body_len && io_send_all(c, t_body, t_body_len) != 0)) {
        conn_drop(h, c);
        return -1;
    }

    char hdr[8192];
    size_t hl = 0;
    char *end = NULL;
    while (!end) {
        if (hl + 1 >= sizeof(hdr)) {
            mt_log("[http] header too large");
            conn_drop(h, c);
            return -1;
        }
        ssize_t r = io_recv(c, hdr + hl, sizeof(hdr) - 1 - hl);
        if (r <= 0) {
            int e = errno;
            if (!conn_drop(h, c))
                mt_log("[http] connection closed while reading header (%s)", r < 0 ? strerror(e) : "EOF");
            return -1;
        }
        hl += (size_t) r;
        hdr[hl] = 0;
        end = strstr(hdr, "\r\n\r\n");
    }
    size_t head_len = (size_t) (end - hdr) + 4;
    if (t_any) snprintf(t_head, sizeof(t_head), "%.*s", (int) (end - hdr), hdr);
    c->pre_len = hl - head_len;
    if (c->pre_len > sizeof(c->pre)) {
        c->pre_len = sizeof(c->pre);
    }
    memcpy(c->pre, hdr + head_len, c->pre_len);
    c->pre_pos = 0;
    *end = 0;

    c->status = 0;
    c->location[0] = 0;
    c->content_range[0] = 0;
    sscanf(hdr, "HTTP/%*d.%*d %d", &c->status);
    c->content_type[0] = 0;
    c->content_length = c->total_size = -1;
    if (www && wwwlen) {
        www[0] = 0;
    }
    for (char *line = strstr(hdr, "\r\n"); line; line = strstr(line + 2, "\r\n")) {
        char *l = line + 2;
        if (strncasecmp(l, "Content-Type:", 13) == 0) {
            sscanf(l + 13, " %63[^\r\n;]", c->content_type);
        } else if (strncasecmp(l, "Content-Length:", 15) == 0) {
            c->content_length = strtoll(l + 15, NULL, 10);
        } else if (strncasecmp(l, "Content-Range:", 14) == 0) {
            sscanf(l + 14, " %95[^\r\n]", c->content_range);
            const char *sl = strchr(l, '/');
            if (sl && sl[1] != '*') c->total_size = strtoll(sl + 1, NULL, 10);
        } else if (strncasecmp(l, "Location:", 9) == 0) {
            sscanf(l + 9, " %2047[^\r\n]", c->location);
        } else if (strncasecmp(l, "Transfer-Encoding:", 18) == 0) {
            char te[64] = "";
            sscanf(l + 18, " %63[^\r\n]", te);
            c->chunked = strcasestr(te, "chunked") != NULL;
        } else if (www && strncasecmp(l, "WWW-Authenticate:", 17) == 0) {
            const char *v = l + 17;
            while (*v == ' ') v++;
            /* prefer Digest if several schemes are offered */
            if (www[0] == 0 || strncasecmp(v, "Digest", 6) == 0) {
                size_t i = 0;
                while (v[i] && v[i] != '\r' && i + 1 < wwwlen) {
                    www[i] = v[i];
                    i++;
                }
                www[i] = 0;
            }
        }
    }
    return 0;
}

static __thread int t_quiet;

int http_open(const char *url, const char *user, const char *pass, http_conn *out) {
    return http_open_range(url, user, pass, -1, out);
}

int http_open_quiet(const char *url, const char *user, const char *pass, http_conn *out) {
    t_quiet = 1;
    int r = http_open_range(url, user, pass, -1, out);
    t_quiet = 0;
    return r;
}

int http_open_hdr(const char *url, const char *extra_headers, const http_hook *hook, http_conn *out) {
    t_extra = extra_headers;
    int r = http_open_ex(url, NULL, NULL, -1, hook, out);
    t_extra = NULL;
    return r;
}

int http_open_range(const char *url, const char *user, const char *pass, long long from, http_conn *out) {
    return http_open_ex(url, user, pass, from, NULL, out);
}

int http_open_span(const char *url, long long from, long long to, http_conn *out) {
    t_range_to = to;
    t_quiet = 1;
    int r = http_open_ex(url, NULL, NULL, from, NULL, out);
    t_quiet = 0;
    t_range_to = -1;
    if (r == 0 && out->status != 206) { /* the whole file would be too much */
        mt_log("[http] server ignores Range");
        http_close(out);
        return -1;
    }
    return r;
}

/* replace the query's password parameters (pass=, password=) in the text with *** */
static void mask_query(char *s, size_t n) {
    static const char *const keys[] = {"pass=", "password="};
    for (char *q = strchr(s, '?'); q && *q; q = strchr(q + 1, '&')) {
        for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
            size_t kl = strlen(keys[k]);
            if (strncmp(q + 1, keys[k], kl)) continue;
            char *v = q + 1 + kl, *end = v + strcspn(v, "&");
            size_t vl = (size_t) (end - v), rl = strlen(end);
            if (vl == 0) break;
            if ((size_t) (v - s) + 3 + rl + 1 > n) { /* does not fit: cut off the rest */
                *v = 0;
                rl = 0;
                end = v;
            }
            memmove(v + 3, end, rl + 1);
            memcpy(v, "***", 3);
            break;
        }
    }
}

void http_url_redact(const char *url, char *out, size_t n) {
    const char *p = strstr(url, "://");
    const char *host = p ? p + 3 : url;
    const char *slash = strchr(host, '/');
    const char *at = strchr(host, '@');
    if (p && at && (!slash || at < slash))
        snprintf(out, n, "%.*s%s", (int) (host - url), url, at + 1);
    else snprintf(out, n, "%s", url);
    mask_query(out, n);
}

void http_url_userinfo(const char *user, const char *pass, char *out, size_t n) {
    size_t o = 0;
    const char *parts[2] = {user ? user : "", pass ? pass : ""};
    out[0] = 0;
    if (!parts[0][0]) return;
    for (int k = 0; k < 2 && o + 1 < n; k++) {
        if (k) out[o++] = ':';
        for (const unsigned char *c = (const unsigned char *) parts[k]; *c && o + 4 < n; c++) {
            if ((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9') || *c == '-' ||
                *c == '_' || *c == '.' || *c == '~')
                out[o++] = (char) *c;
            else o += (size_t) snprintf(out + o, n - o, "%%%02X", *c);
        }
    }
    if (o + 1 < n) out[o++] = '@';
    out[o] = 0;
}

int http_request(const char *method, const char *url, const char *extra_headers, const void *body, size_t len,
                 const http_hook *hook, http_conn *out) {
    t_method = method;
    t_body = body;
    t_body_len = len;
    t_extra = extra_headers;
    t_any = 1;
    t_head[0] = 0;
    int r = http_open_ex(url, NULL, NULL, -1, hook, out);
    t_method = NULL;
    t_body = NULL;
    t_body_len = 0;
    t_extra = NULL;
    t_any = 0;
    return r;
}

/* resolve a redirect target relative to base */
void http_join_url(const char *base, const char *ref, char *out, size_t n) {
    if (strstr(ref, "://")) {
        snprintf(out, n, "%s", ref);
        return;
    }
    const char *hs = strstr(base, "://");
    const char *root = hs ? strchr(hs + 3, '/') : NULL;
    size_t rl = root ? (size_t) (root - base) : strlen(base);
    if (ref[0] == '/' && ref[1] == '/') {
        snprintf(out, n, "%.*s%s", hs ? (int) (hs - base + 1) : 0, base, ref);
    } else if (ref[0] == '/') {
        snprintf(out, n, "%.*s%s", (int) rl, base, ref);
    } else {
        const char *q = strchr(base, '?');
        size_t bl = q ? (size_t) (q - base) : strlen(base);
        const char *sl = base + bl;
        while (sl > base + rl && sl[-1] != '/') sl--;
        snprintf(out, n, "%.*s%s", (int) (sl - base), base, ref);
    }
}

int http_open_ex(const char *url, const char *user, const char *pass, long long from, const http_hook *hook,
                 http_conn *out) {
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    url_parts u;
    char cur[4096];
    snprintf(cur, sizeof(cur), "%s", url);
    char www[1024];
    for (int hop = 0;; hop++) {
        if (parse_url(cur, &u) != 0) {
            mt_log("[http] invalid URL: %.80s", cur);
            return -1;
        }
        if (user && *user) snprintf(u.user, sizeof(u.user), "%s", user);
        if (pass && *pass) snprintf(u.pass, sizeof(u.pass), "%s", pass);
        if (do_request(&u, NULL, from, hook, out, www, sizeof(www)) != 0) {
            return -1;
        }
        int st = out->status;
        if ((st == 301 || st == 302 || st == 303 || st == 307 || st == 308) && out->location[0] && hop < 5) {
            char next[4096];
            http_join_url(cur, out->location, next, sizeof(next));
            if (!t_quiet) mt_log("[http] %d -> %.200s", st, next);
            conn_drop(hook, out);
            /* 301-303 after POST: continue with GET without body (like browsers) */
            if (st != 307 && st != 308 && t_body) {
                t_method = NULL;
                t_body = NULL;
                t_body_len = 0;
            }
            snprintf(cur, sizeof(cur), "%s", next);
            continue;
        }
        break;
    }
    snprintf(out->url, sizeof(out->url), "%s", cur);
    if (!t_quiet || ((out->status < 200 || out->status >= 300) && !(out->status == 401 && u.user[0]))) {
        char lp[sizeof(u.path)];
        snprintf(lp, sizeof(lp), "%s", u.path);
        mask_query(lp, sizeof(lp));
        mt_log("[http] %s:%s%s -> %d %s", u.host, u.port, lp, out->status, out->content_type);
    }
    if (out->status == 401 && u.user[0]) {
        conn_drop(hook, out);
        char auth[2048];
        if (strncasecmp(www, "Digest", 6) == 0) {
            build_digest(www, &u, auth, sizeof(auth));
        } else {
            char up[160], enc[256];
            snprintf(up, sizeof(up), "%s:%s", u.user, u.pass);
            b64(up, enc, sizeof(enc));
            snprintf(auth, sizeof(auth), "Authorization: Basic %s\r\n", enc);
        }
        if (do_request(&u, auth, from, hook, out, NULL, 0) != 0) {
            return -1;
        }
        if (!t_quiet || out->status < 200 || out->status >= 300)
            mt_log("[http] with %s auth -> %d %s", strncasecmp(www, "Digest", 6) == 0 ? "Digest" : "Basic",
                   out->status, out->content_type);
    }
    if (t_any && out->status >= 100) return 0; /* http_request: error responses too */
    if (out->status < 200 || out->status >= 300 || (from > 0 && out->status != 206)) {
        if (from > 0 && out->status == 200) mt_log("[http] server ignores Range");
        hook_fd(hook, -1);
        http_close(out);
        return -1;
    }
    if (out->total_size < 0 && out->status == 200) out->total_size = out->content_length;
    return 0;
}

/* raw data: first what came with the header, then directly from the socket */
static ssize_t raw_read(http_conn *c, void *buf, size_t len) {
    if (c->pre_pos < c->pre_len) {
        size_t n = c->pre_len - c->pre_pos;
        if (n > len) n = len;
        memcpy(buf, c->pre + c->pre_pos, n);
        c->pre_pos += n;
        return (ssize_t) n;
    }
    if (c->fd < 0) {
        return -1;
    }
    return io_recv(c, buf, len);
}

/* one byte (for chunk headers), buffered via pre */
static int raw_getc(http_conn *c) {
    if (c->pre_pos >= c->pre_len) {
        if (c->fd < 0) return -1;
        ssize_t r = io_recv(c, c->pre, sizeof(c->pre));
        if (r <= 0) return -1;
        c->pre_len = (size_t) r;
        c->pre_pos = 0;
    }
    return c->pre[c->pre_pos++];
}

ssize_t http_read(http_conn *c, void *buf, size_t len) {
    if (!c->chunked) return raw_read(c, buf, len);
    for (;;) {
        if (c->chunk_eof) return 0;
        if (c->chunk_left > 0) {
            ssize_t r = raw_read(c, buf, len < c->chunk_left ? len : c->chunk_left);
            if (r > 0) {
                c->chunk_left -= (size_t) r;
                if (c->chunk_left == 0) c->chunk_crlf = 1;
            }
            return r;
        }
        if (c->chunk_crlf) { /* CRLF after the chunk data */
            int a = raw_getc(c), b = a == '\r' ? raw_getc(c) : a;
            if (b < 0) return -1;
            c->chunk_crlf = 0;
        }
        /* chunk header: size hex [; extension] CRLF */
        char line[64];
        size_t n = 0;
        int ch;
        while ((ch = raw_getc(c)) >= 0 && ch != '\n') {
            if (n + 1 < sizeof(line)) line[n++] = (char) ch;
        }
        if (ch < 0) return -1;
        if (n && line[n - 1] == '\r') n--;
        line[n] = 0;
        if (n == 0) continue; /* tolerate an empty line */
        char *end;
        size_t size = (size_t) strtoul(line, &end, 16);
        if (end == line) return -1; /* no hex value: stream corrupt */
        if (size == 0) {         /* last chunk */
            c->chunk_eof = 1;
            return 0;
        }
        c->chunk_left = size;
    }
}

void http_close(http_conn *c) {
    if (c->ssl) SSL_free((SSL *) c->ssl);
    c->ssl = NULL;
    if (c->fd >= 0) {
        shutdown(c->fd, SHUT_RDWR);
        close(c->fd);
    }
    c->fd = -1;
}
