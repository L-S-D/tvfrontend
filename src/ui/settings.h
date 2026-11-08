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
} settings_actions;

void settings_init(const settings_actions *a);

void settings_open(void);
void settings_close(void);
int settings_visible(void);

/* keys (app_key from keys.h); open: consumes everything, also repeats of
 * the arrow keys */
int settings_key(int key);
int settings_pointer(int ev, int x, int y);
void settings_wheel(int dir);
