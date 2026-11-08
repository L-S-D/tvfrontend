/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Relay on 127.0.0.1 for the country egress (egress.c):
 *  - HTTP proxy for our own HbbTV renderer (WebKit): CONNECT tunnel
 *    (https) and requests with a full URL (http); the way out is
 *    chosen per target host according to the rules (http_upstream).
 *  - Media for the TV's media server, which knows no proxy:
 *    http://127.0.0.1:<port>/r/<base64url(scheme://host)>/<path> fetches
 *    scheme://host/<path> according to the rules and passes it through (Range,
 *    Content-Range). Absolute addresses in DASH manifests are rewritten to
 *    point at the relay, root-relative paths go to the most recently used
 *    origin.
 *  - License requests of the DRM client: .../l/<base64url>/<path> (POST according to
 *    the rules, optionally with a full capture in the log).
 *  - TVF files for the HbbTV pages: .../f/<name> (registered ones only,
 *    e.g. dash.js for the A/V objects of the OIPF replacement).
 * One thread per connection.
 */
#pragma once

#include <stddef.h>

/* Listens on 127.0.0.1:port (0 = any). 0 = ok */
int relay_start(int port);
int relay_port(void);

/* Address for the media server (out empty if the relay is not running) */
void relay_url(const char *url, char *out, size_t n);

/* License address for the TV's DRM client: .../l/<origin>/<path> - the
 * relay forwards the POST according to the egress rules (hbbdrm_proxy) */
void relay_license_url(const char *url, char *out, size_t n);

/* License requests: 1 = request and response in full in the log, 0 = one
 * line per request and response (error text on error status) */
void relay_license_dump(int on);

/* Serve a file at http://127.0.0.1:<port>/f/<name> (up to 8 of them);
 * only names registered this way, no paths from the request */
void relay_add_file(const char *name, const char *path, const char *type);
