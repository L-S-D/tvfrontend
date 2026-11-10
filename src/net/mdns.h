/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Minimal mDNS responder (RFC 6762/6763) for DNS-SD services - so that
 * Kore finds TVF like a Kodi (_xbmc-jsonrpc-h._tcp, _xbmc-jsonrpc._tcp). Own
 * thread: announces three times at startup and answers queries for the
 * service types (PTR), the instance (SRV/TXT) and the host name (A), also
 * simple unicast queries (source port not 5353).
 */
#pragma once

typedef struct {
    const char *type; /* "_xbmc-jsonrpc-h._tcp" */
    int port;
} mdns_service;

/* instance: displayed name (one label, UTF-8), ip: own IPv4 address.
 * The values are copied. 0 = running */
int mdns_start(const char *instance, const char *ip, const mdns_service *s, int n);

/*
 * TVHeadend servers in the local network: tvh announces itself (Avahi) as
 * _htsp._tcp (HTSP port) and _http._tcp (web port, TXT path=<webroot>) under
 * the same instance name. The search asks from a port of its own (RFC 6762
 * 6.7: the responders answer by unicast, the responder above keeps 5353),
 * three times within the wait, and collects PTR, SRV, TXT and A records.
 * Own thread.
 */
typedef struct {
    char name[64];      /* instance: "Tvheadend" */
    char hostname[64];  /* SRV target: "server.local" */
    char addr[16];      /* IPv4 address (A record, else the sender) */
    int htsp_port;      /* 9982 */
    int http_port;      /* 0 = no web interface announced */
    char http_root[64]; /* path of the web interface: "/" or the webroot */
} mdns_tvh;

#define MDNS_TVH_MAX 8

/* start a search of wait_ms in the background; 0 = started (or one runs) */
int mdns_find_tvh(int wait_ms);
/* main thread: -1 = searching (or none started), else the number of servers
 * found (at most max copied to out); a result is reported once */
int mdns_find_tvh_result(mdns_tvh *out, int max);
