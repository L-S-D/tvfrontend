/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE /* strcasestr */
#include "relay.h"

#include "http.h"
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
#include <sys/time.h>
#include <unistd.h>

#define HEAD_MAX 16384
#define MPD_MAX (4u << 20)

static int g_port;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_last_origin[256]; /* for root-relative paths from manifests */

/* files for /f/<name> (relay_add_file) */
#define FILES_MAX 8
static struct {
    char name[64], path[512], type[64];
} g_files[FILES_MAX];
static int g_nfiles;

/* ---------------- base64url ---------------- */

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static void b64url_enc(const char *in, char *out, size_t n) {
    size_t len = strlen(in), o = 0;
    for (size_t i = 0; i < len && o + 5 < n; i += 3) {
        uint32_t v = (uint32_t) (unsigned char) in[i] << 16;
        if (i + 1 < len) v |= (uint32_t) (unsigned char) in[i + 1] << 8;
        if (i + 2 < len) v |= (uint32_t) (unsigned char) in[i + 2];
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        if (i + 1 < len) out[o++] = B64[(v >> 6) & 63];
        if (i + 2 < len) out[o++] = B64[v & 63];
    }
    out[o] = 0;
}

static int b64val(char c) {
    const char *p = c ? strchr(B64, c) : NULL;
    return p ? (int) (p - B64) : -1;
}

static int b64url_dec(const char *in, size_t len, char *out, size_t n) {
    size_t o = 0;
    uint32_t acc = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        int v = b64val(in[i]);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o + 1 >= n) return -1;
            out[o++] = (char) ((acc >> bits) & 0xFF);
        }
    }
    out[o] = 0;
    return 0;
}

void relay_url(const char *url, char *out, size_t n) {
    out[0] = 0;
    if (!g_port) return;
    const char *hs = strstr(url, "://");
    if (!hs) return;
    const char *path = strchr(hs + 3, '/');
    char origin[256], enc[400];
    snprintf(origin, sizeof(origin), "%.*s", (int) (path ? path - url : (long) strlen(url)), url);
    b64url_enc(origin, enc, sizeof(enc));
    snprintf(out, n, "http://127.0.0.1:%d/r/%s%s", g_port, enc, path ? path : "/");
}

void relay_license_url(const char *url, char *out, size_t n) {
    relay_url(url, out, n);
    char *r = strstr(out, "/r/");
    if (r) r[1] = 'l';
}

int relay_port(void) { return g_port; }

/* ---------------- Helpers ---------------- */

static int send_all(int fd, const void *b, size_t n) {
    const char *p = b;
    while (n) {
        ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        n -= (size_t) w;
    }
    return 0;
}

static void reply(int fd, int status, const char *text) {
    char b[160];
    int n = snprintf(b, sizeof(b), "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status, text);
    send_all(fd, b, (size_t) n);
}

/* copy both directions until one side closes */
static void pump(int a, int b, const char *pre, size_t prelen) {
    if (prelen && send_all(b, pre, prelen) != 0) return;
    char buf[32768];
    struct pollfd p[2] = {{a, POLLIN, 0}, {b, POLLIN, 0}};
    for (;;) {
        int r = poll(p, 2, 120000);
        if (r <= 0) break;
        for (int i = 0; i < 2; i++) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = recv(p[i].fd, buf, sizeof(buf), 0);
            if (n <= 0) return;
            if (send_all(p[1 - i].fd, buf, (size_t) n) != 0) return;
        }
    }
}

/* header line (without colon) from the request header, out empty if none */
static void header(const char *head, const char *name, char *out, size_t n) {
    out[0] = 0;
    size_t nl = strlen(name);
    for (const char *l = strstr(head, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
        if (!strncasecmp(l + 2, name, nl) && l[2 + nl] == ':') {
            const char *v = l + 3 + nl;
            while (*v == ' ') v++;
            size_t i = 0;
            while (v[i] && v[i] != '\r' && i + 1 < n) {
                out[i] = v[i];
                i++;
            }
            out[i] = 0;
            return;
        }
    }
}

/* ---------------- Proxy for WebKit ---------------- */

static void do_connect(int fd, const char *target) {
    char host[256], port[16] = "443";
    const char *c = strrchr(target, ':');
    snprintf(host, sizeof(host), "%.*s", (int) (c ? c - target : (long) strlen(target)), target);
    if (c) snprintf(port, sizeof(port), "%s", c + 1);
    int vp = 0, up = http_upstream(host, port, 1, &vp);
    if (up < 0) {
        reply(fd, 502, "Bad Gateway");
        return;
    }
    static const char ok[] = "HTTP/1.1 200 Connection established\r\n\r\n";
    if (send_all(fd, ok, sizeof(ok) - 1) == 0) pump(fd, up, NULL, 0);
    close(up);
}

/* "GET http://host[:port]/path HTTP/1.1": on to the proxy (full URL)
 * or direct (path); header without proxy lines, connection closed afterwards */
static void do_forward(int fd, const char *method, const char *target, const char *ver, char *head, size_t headlen,
                       const char *extra, size_t extralen) {
    char host[256], port[16] = "80";
    const char *h = target + 7, *path = strchr(h, '/');
    size_t hl = path ? (size_t) (path - h) : strlen(h);
    snprintf(host, sizeof(host), "%.*s", (int) hl, h);
    char *colon = strrchr(host, ':');
    if (colon) {
        snprintf(port, sizeof(port), "%s", colon + 1);
        *colon = 0;
    }
    int vp = 0, up = http_upstream(host, port, 0, &vp);
    if (up < 0) {
        reply(fd, 502, "Bad Gateway");
        return;
    }
    /* new header: request line + remaining lines without proxy/connection lines */
    char *nh = malloc(headlen + 2048 + 64);
    if (!nh) {
        close(up);
        return;
    }
    int o = snprintf(nh, 2048, "%s %s %s\r\n", method, vp ? target : (path ? path : "/"), ver);
    if (o < 0 || o >= 2048) {
        /* too long: if truncated, the rest of the buffer would have gone out
         * uninitialized (snprintf returns the full length) */
        free(nh);
        close(up);
        reply(fd, 414, "URI Too Long");
        return;
    }
    const char *l = strstr(head, "\r\n");
    while (l && l[2] && strncmp(l, "\r\n\r\n", 4)) {
        const char *s = l + 2, *e = strstr(s, "\r\n");
        if (!e) break;
        if (strncasecmp(s, "Proxy-", 6) && strncasecmp(s, "Connection:", 11) && strncasecmp(s, "Keep-Alive:", 11)) {
            memcpy(nh + o, s, (size_t) (e - s) + 2);
            o += (int) (e - s) + 2;
        }
        l = e;
    }
    o += snprintf(nh + o, 64, "Connection: close\r\n\r\n");
    if (send_all(up, nh, (size_t) o) == 0) pump(fd, up, extra, extralen);
    free(nh);
    close(up);
}

/* ---------------- Media for the media server ---------------- */

/* rewrite absolute addresses in manifest attributes/BaseURL to point at the relay */
static char *rewrite_mpd(const char *in, size_t len, size_t *outlen) {
    size_t cap = len * 2 + 4096, o = 0;
    char *out = malloc(cap);
    if (!out) return NULL;
    for (size_t i = 0; i < len;) {
        int at_url = 0;
        if (!strncmp(in + i, "http://", 7) || !strncmp(in + i, "https://", 8)) {
            /* only in BaseURL/Location text and media/initialization/sourceURL
             * attributes - namespaces (xmlns="http://...") stay */
            size_t b = i;
            while (b > 0 && in[b - 1] != '>' && in[b - 1] != '"' && in[b - 1] != '\'') b--;
            if (b > 0 && in[b - 1] == '>') {
                size_t t = b - 1;
                while (t > 0 && in[t] != '<') t--;
                at_url = !strncmp(in + t, "<BaseURL", 8) || !strncmp(in + t, "<Location", 9);
            } else if (b > 0) {
                size_t a = b - 1;
                while (a > 0 && in[a - 1] != ' ') a--;
                at_url = !strncmp(in + a, "media=", 6) || !strncmp(in + a, "initialization=", 15) ||
                         !strncmp(in + a, "sourceURL=", 10);
            }
        }
        if (!at_url) {
            if (o + 1 >= cap) break;
            out[o++] = in[i++];
            continue;
        }
        size_t e = i;
        while (e < len && in[e] != '"' && in[e] != '\'' && in[e] != '<' && in[e] != ' ') e++;
        char url[2048], rel[2600];
        snprintf(url, sizeof(url), "%.*s", (int) (e - i), in + i);
        relay_url(url, rel, sizeof(rel));
        size_t rl = strlen(rel);
        if (o + rl + 1 >= cap) break;
        memcpy(out + o, rel, rl);
        o += rl;
        i = e;
    }
    out[o] = 0;
    *outlen = o;
    return out;
}

static void do_media(int fd, const char *method, const char *target, const char *head) {
    char origin[256] = "", url[3000];
    const char *rest = target;
    if (!strncmp(target, "/r/", 3)) {
        const char *s = target + 3, *sl = strchr(s, '/');
        size_t el = sl ? (size_t) (sl - s) : strlen(s);
        if (b64url_dec(s, el, origin, sizeof(origin)) != 0) {
            reply(fd, 400, "Bad Request");
            return;
        }
        rest = sl ? sl : "/";
        pthread_mutex_lock(&g_lock);
        snprintf(g_last_origin, sizeof(g_last_origin), "%s", origin);
        pthread_mutex_unlock(&g_lock);
    } else {
        /* root-relative from a manifest: most recently used origin */
        pthread_mutex_lock(&g_lock);
        snprintf(origin, sizeof(origin), "%s", g_last_origin);
        pthread_mutex_unlock(&g_lock);
        if (!origin[0]) {
            reply(fd, 404, "Not Found");
            return;
        }
    }
    snprintf(url, sizeof(url), "%s%s", origin, rest);
    char range[128], ua[256], extra[512];
    header(head, "Range", range, sizeof(range));
    header(head, "User-Agent", ua, sizeof(ua));
    int n = 0;
    extra[0] = 0;
    if (range[0]) n += snprintf(extra + n, sizeof(extra) - (size_t) n, "Range: %s\r\n", range);
    if (ua[0]) snprintf(extra + n, sizeof(extra) - (size_t) n, "User-Agent: %s\r\n", ua);
    http_conn c;
    if (http_open_hdr(url, extra, NULL, &c) != 0) {
        int st = c.status ? c.status : 502;
        reply(fd, st, st == 404 ? "Not Found" : "Upstream Error");
        return;
    }
    int is_mpd = strcasestr(c.content_type, "dash+xml") != NULL || strstr(rest, ".mpd") != NULL;
    char hd[1024];
    if (is_mpd && strcasecmp(method, "HEAD") != 0) {
        size_t cap = 256 * 1024, len = 0;
        char *b = malloc(cap);
        while (b) {
            if (len + 8192 > cap) {
                if (cap >= MPD_MAX) break;
                char *nb = realloc(b, cap * 2);
                if (!nb) break;
                b = nb;
                cap *= 2;
            }
            ssize_t r = http_read(&c, b + len, cap - len - 1);
            if (r <= 0) break;
            len += (size_t) r;
        }
        size_t ol = 0;
        char *mpd = b ? rewrite_mpd(b, len, &ol) : NULL;
        free(b);
        if (mpd) {
            int hn = snprintf(hd, sizeof(hd),
                              "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                              c.content_type[0] ? c.content_type : "application/dash+xml", ol);
            if (send_all(fd, hd, (size_t) hn) == 0) send_all(fd, mpd, ol);
            free(mpd);
        } else {
            reply(fd, 502, "Upstream Error");
        }
        http_close(&c);
        return;
    }
    int hn = snprintf(hd, sizeof(hd), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nAccept-Ranges: bytes\r\n", c.status,
                      c.status == 206 ? "Partial Content" : "OK", c.content_type[0] ? c.content_type : "application/octet-stream");
    if (c.content_length >= 0) hn += snprintf(hd + hn, sizeof(hd) - (size_t) hn, "Content-Length: %lld\r\n", c.content_length);
    if (c.content_range[0]) hn += snprintf(hd + hn, sizeof(hd) - (size_t) hn, "Content-Range: %s\r\n", c.content_range);
    hn += snprintf(hd + hn, sizeof(hd) - (size_t) hn, "Connection: close\r\n\r\n");
    if (send_all(fd, hd, (size_t) hn) == 0 && strcasecmp(method, "HEAD") != 0) {
        char buf[65536];
        for (;;) {
            ssize_t r = http_read(&c, buf, sizeof(buf));
            if (r <= 0 || send_all(fd, buf, (size_t) r) != 0) break;
        }
    }
    http_close(&c);
}

/* ---------------- License requests (diagnostics) ---------------- */

#define LIC_MAX (256u << 10)

static int g_lic_dump; /* request and response in full in the log */

void relay_license_dump(int on) { g_lic_dump = on; }

/* text in pieces into the log (lines up to 2 KB), line breaks as spaces */
static void log_block(const char *tag, const char *data, size_t len) {
    char buf[1201];
    for (size_t i = 0; i < len; i += 1200) {
        size_t k = len - i < 1200 ? len - i : 1200;
        memcpy(buf, data + i, k);
        buf[k] = 0;
        for (size_t j = 0; j < k; j++)
            if (buf[j] == '\r' || buf[j] == '\n' || buf[j] == 0) buf[j] = ' ';
        mt_log("[lic] %s %zu/%zu %s", tag, i, len, buf);
    }
}

/* http://127.0.0.1:<port>/l/<base64url(scheme://host)>/<path>: POST of the
 * TV's DRM client (PlayReady challenge) to scheme://host/<path> according to the
 * egress rules, request (header, body) and response (status, header, body)
 * in full in the log - for capturing and reproducing on a PC */
static void do_license(int fd, const char *method, const char *target, const char *head, const char *pre,
                       size_t prelen) {
    char origin[256], url[3000];
    const char *s = target + 3, *sl = strchr(s, '/');
    size_t el = sl ? (size_t) (sl - s) : strlen(s);
    if (b64url_dec(s, el, origin, sizeof(origin)) != 0) {
        reply(fd, 400, "Bad Request");
        return;
    }
    snprintf(url, sizeof(url), "%s%s", origin, sl ? sl : "/");
    char clen_s[32], ctype[160], soap[300], ua[300], accept[160];
    header(head, "Content-Length", clen_s, sizeof(clen_s));
    header(head, "Content-Type", ctype, sizeof(ctype));
    header(head, "SOAPAction", soap, sizeof(soap));
    header(head, "User-Agent", ua, sizeof(ua));
    header(head, "Accept", accept, sizeof(accept));
    long long clen = clen_s[0] ? atoll(clen_s) : 0;
    if (clen < 0 || (unsigned long long) clen > LIC_MAX) {
        reply(fd, 413, "Payload Too Large");
        return;
    }
    char *body = malloc((size_t) clen + 1);
    if (!body) {
        reply(fd, 500, "Internal Error");
        return;
    }
    size_t have = prelen < (size_t) clen ? prelen : (size_t) clen;
    memcpy(body, pre, have);
    while (have < (size_t) clen) {
        ssize_t r = recv(fd, body + have, (size_t) clen - have, 0);
        if (r <= 0) break;
        have += (size_t) r;
    }
    body[have] = 0;
    /* request header without the first line, on one line */
    const char *hl = strstr(head, "\r\n");
    char hcopy[2048];
    snprintf(hcopy, sizeof(hcopy), "%s", hl ? hl + 2 : "");
    for (char *p = hcopy; *p; p++)
        if (*p == '\r' || *p == '\n') *p = ' ';
    if (g_lic_dump) {
        mt_log("[lic] %s %s (%zu bytes)", method, url, have);
        mt_log("[lic] header: %s", hcopy);
        log_block(">", body, have);
    } else {
        mt_log("[lic] %s %.160s (%zu bytes)", method, url, have);
    }
    char extra[1200];
    int n = 0;
    extra[0] = 0;
    if (ctype[0]) n += snprintf(extra + n, sizeof(extra) - (size_t) n, "Content-Type: %s\r\n", ctype);
    if (soap[0]) n += snprintf(extra + n, sizeof(extra) - (size_t) n, "SOAPAction: %s\r\n", soap);
    if (ua[0]) n += snprintf(extra + n, sizeof(extra) - (size_t) n, "User-Agent: %s\r\n", ua);
    if (accept[0]) snprintf(extra + n, sizeof(extra) - (size_t) n, "Accept: %s\r\n", accept);
    http_conn c;
    if (http_request(method, url, extra, body, have, NULL, &c) != 0) {
        mt_log("[lic] no response (status %d)", c.status);
        reply(fd, c.status ? c.status : 502, "Upstream Error");
        free(body);
        return;
    }
    free(body);
    char *resp = malloc(LIC_MAX + 1);
    size_t rl = 0;
    while (resp && rl < LIC_MAX) {
        ssize_t r = http_read(&c, resp + rl, LIC_MAX - rl);
        if (r <= 0) break;
        rl += (size_t) r;
    }
    char rhead[4096];
    snprintf(rhead, sizeof(rhead), "%s", http_last_head());
    for (char *p = rhead; *p; p++)
        if (*p == '\r' || *p == '\n') *p = ' ';
    mt_log("[lic] response %d %s, %zu bytes", c.status, c.content_type, rl);
    if (g_lic_dump) {
        mt_log("[lic] response header: %s", rhead);
        if (resp) log_block("<", resp, rl);
    } else if (resp && (c.status < 200 || c.status >= 300)) {
        log_block("<", resp, rl < 1200 ? rl : 1200); /* error text of the license server */
    }
    char hd[512];
    int hn = snprintf(hd, sizeof(hd), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                      c.status, c.status == 200 ? "OK" : "Error", c.content_type[0] ? c.content_type : "text/xml", rl);
    if (send_all(fd, hd, (size_t) hn) == 0 && resp && rl) send_all(fd, resp, rl);
    free(resp);
    http_close(&c);
}

/* ---------------- Files ---------------- */

void relay_add_file(const char *name, const char *path, const char *type) {
    pthread_mutex_lock(&g_lock);
    if (g_nfiles < FILES_MAX && name && path) {
        snprintf(g_files[g_nfiles].name, sizeof(g_files[g_nfiles].name), "%s", name);
        snprintf(g_files[g_nfiles].path, sizeof(g_files[g_nfiles].path), "%s", path);
        snprintf(g_files[g_nfiles].type, sizeof(g_files[g_nfiles].type), "%s", type ? type : "application/octet-stream");
        g_nfiles++;
    }
    pthread_mutex_unlock(&g_lock);
}

/* GET /f/<name>: the whole file (the pages load it via <script>), may be cached */
static void do_file(int fd, const char *method, const char *name) {
    char path[512] = "", type[64] = "";
    size_t nl = strcspn(name, "?#");
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nfiles; i++)
        if (strlen(g_files[i].name) == nl && !strncmp(g_files[i].name, name, nl)) {
            snprintf(path, sizeof(path), "%s", g_files[i].path);
            snprintf(type, sizeof(type), "%s", g_files[i].type);
        }
    pthread_mutex_unlock(&g_lock);
    FILE *f = path[0] ? fopen(path, "rb") : NULL;
    if (!f) {
        reply(fd, 404, "Not Found");
        return;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char hd[300];
    int hn = snprintf(hd, sizeof(hd),
                      "HTTP/1.1 200 OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nCache-Control: max-age=86400\r\n"
                      "Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n",
                      type, n > 0 ? n : 0);
    int ok = send_all(fd, hd, (size_t) hn) == 0;
    if (ok && strcasecmp(method, "HEAD")) {
        char buf[16384];
        size_t r;
        while (ok && (r = fread(buf, 1, sizeof(buf), f)) > 0) ok = send_all(fd, buf, r) == 0;
    }
    fclose(f);
}

/* ---------------- Connections ---------------- */

static void *serve(void *arg) {
    int fd = (int) (intptr_t) arg;
    struct timeval tv = {30, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char *head = malloc(HEAD_MAX + 1);
    size_t hl = 0;
    char *end = NULL;
    while (head && !end && hl < HEAD_MAX) {
        ssize_t r = recv(fd, head + hl, HEAD_MAX - hl, 0);
        if (r <= 0) break;
        hl += (size_t) r;
        head[hl] = 0;
        end = strstr(head, "\r\n\r\n");
    }
    if (end) {
        size_t headlen = (size_t) (end - head) + 4;
        char method[16], target[3000], ver[16];
        if (sscanf(head, "%15s %2999s %15s", method, target, ver) == 3) {
            /* to the relay itself, but as a proxy request (WebKit sends
             * everything with the full URL to the proxy): like a direct fetch */
            char self[48];
            int sl = snprintf(self, sizeof(self), "http://127.0.0.1:%d/", g_port);
            if (!strncasecmp(target, self, (size_t) sl)) memmove(target, target + sl - 1, strlen(target + sl - 1) + 1);
            if (!strcasecmp(method, "CONNECT")) {
                do_connect(fd, target);
            } else if (!strncmp(target, "/f/", 3)) {
                do_file(fd, method, target + 3);
            } else if (!strncasecmp(target, "http://", 7)) {
                head[headlen - 2] = 0; /* header without the blank line for the line loop */
                do_forward(fd, method, target, ver, head, headlen, head + headlen, hl - headlen);
            } else if (!strncmp(target, "/l/", 3)) {
                do_license(fd, method, target, head, head + headlen, hl - headlen);
            } else {
                do_media(fd, method, target, head);
            }
        } else {
            reply(fd, 400, "Bad Request");
        }
    }
    free(head);
    close(fd);
    return NULL;
}

static void *acceptor(void *arg) {
    int ls = (int) (intptr_t) arg;
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) continue;
            usleep(100000);
            continue;
        }
        pthread_t t;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&at, 256 * 1024);
        if (pthread_create(&t, &at, serve, (void *) (intptr_t) fd) != 0) close(fd);
        pthread_attr_destroy(&at);
    }
    return NULL;
}

int relay_start(int port) {
    if (g_port) return 0;
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (ls < 0) return -1;
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons((uint16_t) port);
    if (bind(ls, (struct sockaddr *) &a, sizeof(a)) != 0) {
        a.sin_port = 0; /* in use: any port */
        if (bind(ls, (struct sockaddr *) &a, sizeof(a)) != 0) {
            close(ls);
            return -1;
        }
    }
    socklen_t l = sizeof(a);
    getsockname(ls, (struct sockaddr *) &a, &l);
    if (listen(ls, 32) != 0) {
        close(ls);
        return -1;
    }
    g_port = ntohs(a.sin_port);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &at, acceptor, (void *) (intptr_t) ls);
    pthread_attr_destroy(&at);
    mt_log("[relay] listening on 127.0.0.1:%d", g_port);
    return 0;
}
