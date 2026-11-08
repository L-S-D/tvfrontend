/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Remote control keys, independent of SDL: the UI modules (EPG, dialogs)
 * only get these values. The mapping to SDL is in app/keys.c.
 */
#pragma once

typedef enum {
    KEY_NONE = 0,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_OK,
    KEY_BACK,
    KEY_EXIT,
    KEY_RED,
    KEY_GREEN,
    KEY_YELLOW,
    KEY_BLUE,
    KEY_CH_UP,
    KEY_CH_DOWN,
    KEY_INFO,
    KEY_RECORD,
    KEY_PLAY,
    KEY_PAUSE,
    KEY_PLAYPAUSE,
    KEY_STOP,
    KEY_REWIND,
    KEY_FFWD,
    KEY_DIGIT0 = 100, /* KEY_DIGIT0 + n, n = 0..9 */
} app_key;

#define KEY_IS_DIGIT(k) ((k) >= KEY_DIGIT0 && (k) <= KEY_DIGIT0 + 9)

/* Magic Remote pointer (screen coordinates 1920x1080): move,
 * OK pressed / released. A click triggers on release. */
typedef enum { PTR_MOVE, PTR_DOWN, PTR_UP } ptr_event;

const char *key_name(int key);
