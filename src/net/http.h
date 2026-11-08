/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Minimal HTTP/1.0 GET client for live streams.
 *
 * Why our own: TVHeadend requires (here) digest auth - Basic is rejected
 * with 403 - and for fast start every millisecond between app start and
 * the first TS packet counts. HTTP/1.0 so that nothing is ever chunk-encoded.
 */
#pragma once

#include <stddef.h>
#include <sys/types.h>

typedef struct {
    int fd;
    void *ssl; /* https: SSL connection (OpenSSL), otherwise NULL */
    int status;
    char content_type[64];
    /* Bytes that arrived with the header and already belong to the body. */
    unsigned char pre[8192];
    size_t pre_len;
    size_t pre_pos;
    /* Transfer-Encoding: chunked (e.g. uvicorn even on HTTP/1.0) */
    int chunked;
    size_t chunk_left;
    int chunk_crlf; /* CRLF after the chunk is still pending */
    int chunk_eof;
    long long content_length; /* -1 = unknown */
    long long total_size;     /* Content-Range ".../total", otherwise Content-Length; -1 = unknown */
    char location[2048];      /* Location of a redirect (internal) */
    char content_range[96];   /* Content-Range of the response (relay) */
    char url[4096];           /* last requested address (after redirects) */
} http_conn;

/* https: file with the trusted root certificates (PEM). Without this
 * call: /etc/ssl/certs/ca-certificates.crt (outdated on the TV). Set before
 * the first https fetch. */
void http_set_ca_file(const char *path);

/* Egress per target host (country proxies): fn returns "http://proxy:port"
 * or "" (direct). Applies to all subsequent fetches; https goes as a
 * CONNECT tunnel through the proxy, http with the full URL. */
void http_set_proxy_resolver(const char *(*fn)(const char *host, char *buf, size_t n));

/* Raw connection for host:port according to the rules (relay): tunnel = 1
 * via CONNECT through the proxy or direct; tunnel = 0 to the proxy itself
 * (*via_proxy = 1, then send the full URL) or direct. -1 = error */
int http_upstream(const char *host, const char *port, int tunnel, int *via_proxy);

/* resolve ref (absolute, "//host/..", "/path" or relative) against base */
void http_join_url(const char *base, const char *ref, char *out, size_t n);

/*
 * Opens url ("http(s)://[user[:pass]@]host[:port]/path"), follows up to 5
 * redirects. Returns 0 on status 2xx, otherwise -1 (the status is then in
 * out->status, if known). https verifies the certificate and host name.
 * user/pass may be NULL; then the ones from the URL apply.
 */
int http_open(const char *url, const char *user, const char *pass, http_conn *out);

/* Like http_open, without a log line on success (frequent fetches) */
int http_open_quiet(const char *url, const char *user, const char *pass, http_conn *out);

/* Like http_open, with "Range: bytes=from-" (from < 0: no Range). The server
 * must then return 206; 200 (ignored) counts as an error if from > 0. */
int http_open_range(const char *url, const char *user, const char *pass, long long from, http_conn *out);

/* Range "Range: bytes=from-to" (both inclusive), without a log line on success;
 * only 206 counts. For samples from /dvrfile: a response up to the
 * end of the file counts as "played" for tvh (playcount + 1) */
int http_open_span(const char *url, long long from, long long to, http_conn *out);

/* url without "user:pass@" (for the log) */
void http_url_redact(const char *url, char *out, size_t n);

/* "user:pass@" with %XX for everything except [A-Za-z0-9-_.~] ("" without user);
 * http_open decodes it again */
void http_url_userinfo(const char *user, const char *pass, char *out, size_t n);

/*
 * Cancelling from another thread: http_open_ex reports every socket right
 * after creating it to hook->fd and unregisters it with -1 before closing
 * it. The owner records it under its lock and uses shutdown() to wake a
 * hanging connect() or the wait for the response header (tvh does not
 * answer at all for a channel without reception). If hook->fd returns
 * != 0, the request is cancelled: http_open_ex returns -1 immediately,
 * without an error message. After success the socket stays registered; the
 * owner unregisters it itself before http_close.
 */
typedef struct {
    int (*fd)(void *user, int fd);
    void *user;
} http_hook;

/* Like http_open, with extra header lines ("Range: ...\r\nUser-Agent: ...\r\n") */
int http_open_hdr(const char *url, const char *extra_headers, const http_hook *hook, http_conn *out);

/* Like http_open_range, with hook (NULL = none) */
int http_open_ex(const char *url, const char *user, const char *pass, long long from, const http_hook *hook,
                 http_conn *out);

/* Any method with a body (e.g. POST of a license request), according to the
 * egress rules. Returns 0 as soon as a response is there - even with an
 * error status (out->status); the body can then be read via http_read.
 * 301-303 continue with GET without body, 307/308 with method and body. */
int http_request(const char *method, const char *url, const char *extra_headers, const void *body, size_t len,
                 const http_hook *hook, http_conn *out);

/* Response header of this thread's last http_request (without the blank line) */
const char *http_last_head(void);

/* Like recv(): >0 bytes, 0 at end of connection, <0 on error/timeout. */
ssize_t http_read(http_conn *c, void *buf, size_t len);

void http_close(http_conn *c);
