/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Egress (country) per HbbTV channel and target host, like the rules of the
 * HbbTV gateway - but in TVF: res/hbbtv/egress.json (proxies per country,
 * exceptions per host, rules per channel name, rules per host). Returns
 * the proxy URL for a host; http.c asks for it on every request
 * (http_set_proxy_resolver), and so does the relay (relay.c).
 * Thread-safe (the rules do not change after loading, the channel
 * is under a lock).
 */
#pragma once

#include <stddef.h>

/* Load the rules; host replaces {host} in the proxy addresses. 0 = ok */
int egress_load(const char *path, const char *host);

/* Channel rules from tvh tags (tvsrc.c, via HTSP): a tag whose name starts
 * with "HbbTV-Proxy" applies to its channels before the name rules.
 * egress_tag_proxy: a tag's egress (malloc) from the icon field
 * (host:port, proxy URL, name from "egress" or "direct", "" = direct),
 * with an empty icon from the rest of the name ("HbbTV-Proxy DE"); NULL = not a
 * proxy tag. The tag must be "private" (internal tags are not sent via
 * HTSP) and be listed in the TV user's access entry. */
char *egress_tag_proxy(const char *name, const char *icon);

/* replaces all tag rules: channel name -> proxy ("" = direct) */
void egress_set_tag_rules(const char *const *channels, const char *const *proxies, int n);

/* HbbTV channel whose rule applies (name as in tvh), NULL/"" = none */
void egress_set_channel(const char *name);

/* Proxy for host: "http://proxy:port" in buf, "" = direct; returns buf */
const char *egress_for(const char *host, char *buf, size_t n);

/* Proxy prescribed by the current channel's channel rule ("" = no rule
 * or direct) - for display/log */
const char *egress_channel_proxy(char *buf, size_t n);
