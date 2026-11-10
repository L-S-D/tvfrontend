/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Settings page (green long in live TV): the basic settings of TVF, stored in
 * the preferences file. Choice items cycle with OK/Left/Right, text items open
 * an on-screen keyboard (digits also from the remote). "Save" writes the
 * changed values and asks the app to restart (most settings apply at start).
 * Main thread only.
 */
#pragma once

#include <stddef.h>

typedef struct {
    /* saved: restart TVF so the settings apply */
    void (*restart)(void);
    /* value in effect for a prefs key (stored or default), "" = off/unset */
    void (*current)(const char *key, char *out, size_t n);
    /* search the network for tvh servers: the result comes with
     * settings_servers; NULL = no search row */
    void (*find)(void);
} settings_actions;

/* a tvh server found in the network */
typedef struct {
    char label[128]; /* "Tvheadend - server.local (192.0.2.10)" */
    char host[64];   /* address for tvh.host */
    int http_port, htsp_port;
} settings_server;

void settings_init(const settings_actions *a);

void settings_open(void);
/* open at the row of the prefs key (NULL: the first row, "tvh.find": the
 * server search) with a note above the list (NULL = none) */
void settings_open_at(const char *key, const char *note);
/* result of the search (act.find): the first server goes into address and
 * ports, left/right in the search row picks another one. Also before
 * opening (servers found at start) */
void settings_servers(const settings_server *s, int n);
void settings_close(void);
int settings_visible(void);

/* keys (app_key from keys.h); open: consumes everything, also repeats of
 * the arrow keys */
int settings_key(int key);
int settings_pointer(int ev, int x, int y);
void settings_wheel(int dir);
