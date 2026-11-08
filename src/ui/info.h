/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Info window for the current (and next) programme: the banner enlarged
 * upwards - header as in the banner, title, subtitle, additional details
 * (genre, episode, year, age rating, duration), the whole long text and the
 * image from the tvh EPG, if there is one. Right/Left switches between
 * current and next programme, Up/Down scrolls the text. Open until
 * Back/OK/Yellow. Main thread only.
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint32_t id; /* programme (for REC), 0 = unknown */
    long start, stop;
    char title[256];
    char subtitle[512];
    char meta[256];  /* "Komödie · S2 E5 · 2011 · ab 12 · 27 min" (German UI: comedy, age 12+) */
    char text[4096]; /* long text, possibly with the short text before it */
    char image[512]; /* URL, "" = no image */
} info_event;

typedef struct {
    int number;
    char channel[128];
    char logo[512];
    int have_now, have_next;
    info_event now, next;
} info_data;

void info_init(void);
void info_open(const info_data *d);
void info_close(void);
int info_visible(void);

/* keys (app_key from keys.h): 1 = handled. info_event_id() returns the programme for REC */
int info_key(int key);
void info_wheel(int dir);
/* pointer: a click closes; open: consumes everything */
int info_pointer(int ev, int x, int y);
/* displayed programme */
uint32_t info_event_id(void);
void info_tick(void); /* clock and remaining time */
