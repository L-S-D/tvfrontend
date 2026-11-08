/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Luna calls (webOS system services, e.g. launching an app) via LG's
 * libhelpers: webOS 4 and 25 both have HLunaServiceCall. The library
 * brings its own GLib loop; the reply arrives in its thread.
 * Loaded via dlopen (not in the SDK sysroot; if it is missing, only this fails).
 */
#pragma once

/* reply: JSON of the reply - in the library's thread, not in the main thread */
typedef void (*luna_reply_fn)(const char *reply, void *user);

/* 0 = sent (cb is called exactly once), -1 = not possible (cb is not called) */
int luna_call(const char *uri, const char *payload, luna_reply_fn cb, void *user);

/* Subscription (several replies, e.g. events from com.webos.media/subscribe);
 * NULL = not possible. luna_unsubscribe: no more replies to cb (the
 * context stays alive - the library might still deliver) */
typedef struct luna_sub luna_sub;
luna_sub *luna_subscribe(const char *uri, const char *payload, luna_reply_fn cb, void *user);
void luna_unsubscribe(luna_sub *s);
