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
