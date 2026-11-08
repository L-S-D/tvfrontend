/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Asynchronous HTTP GETs for small responses (JSON): a few worker threads,
 * the result arrives in the main thread from fetch_poll(). For teletext pages,
 * subtitle events and the like.
 */
#pragma once

#include <stddef.h>

/* status: HTTP status (0 = no connection), body NUL-terminated or NULL */
typedef void (*fetch_cb)(void *user, int status, const char *body, size_t len);

/* wake: wakes the main thread (may be NULL) */
void fetch_init(void (*wake)(void));
/* url "local:<path>" goes to the registered handler (in the calling
 * thread); the result arrives from fetch_poll as usual */
void fetch_get(const char *url, fetch_cb cb, void *user);
/* Handler for "local:": returns the status and *body (malloc, NUL-terminated) */
typedef int (*fetch_local_fn)(const char *path, char **body, size_t *len);
void fetch_set_local(fetch_local_fn fn);
void fetch_poll(void);
