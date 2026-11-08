/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Channel banner (bottom), digit entry (top right) and notices (top).
 * Main thread only.
 */
#pragma once

typedef struct {
    char title[256];
    char subtitle[256];
    char plot[1024];  /* synopsis (only "now") */
    long start, stop; /* Unix seconds, 0 = no programme */
} epg_entry;

void osd_init(void);

void osd_set_channel(int number, const char *name, const char *icon_url);
void osd_set_nownext(const epg_entry *now, const epg_entry *next);
void osd_get_nownext(epg_entry *now, epg_entry *next); /* last set */

/* stream info in the banner (like the HTML5 app): video details and audio tracks,
 * the current one highlighted (current, -1 = none). video NULL/"" and n 0 =
 * empty row. Unchanged details redraw nothing. */
void osd_set_streams(const char *video, const char *const *audio, int n, int current);
/* reception of the current channel (tvh, preformatted) */
typedef struct {
    char provider[64];
    int level;            /* quality: 0 unknown, 1 good, 2 weak, 3 bad/none */
    char snr[24];         /* "16,8 dB", "" = none */
    char signal[24];      /* "−65 dBm", "" = none */
    float snr_frac, signal_frac; /* 0..1 for bars */
    char errs[24];        /* "UNC 12", "" = none */
    char ca[40];          /* "Nagra 1830", "" = no decryption reported */
    char ca_time[16];     /* "305 ms" */
    char ca_via[40];      /* reader or "via newcamd" */
    int ca_failed;
    char error[64];       /* message from tvh */
} osd_rx;
/* below the logo: provider, bars for SNR and signal (colour = quality),
 * box for the decryption; NULL = none */
void osd_set_rx(const osd_rx *rx);

/* recording playback: banner with title, date, channel and progress;
 * state e.g. "PAUSE", "SEEK", "" */
void osd_rec_mode(const char *title, const char *subtitle, const char *plot, const char *channel, long start,
                  const char *icon_url);
void osd_rec_progress(double pos, double dur, const char *state);
void osd_live_mode(void);

/* pointer on the progress bar (recording): 1 = hit (any_y: only x
 * counts, while dragging), *frac = fraction 0..1 */
int osd_bar_frac(int x, int y, int any_y, double *frac);

/* timeshift display top right (two rows); l1 NULL = gone */
void osd_tshift(const char *l1, const char *l2);

/* show the target time above the bar (frac < 0: gone) */
void osd_seek_hover(double frac, const char *text);

/* show the banner (ms = 0: until osd_hide) */
void osd_show(int ms);
void osd_hide(void);
int osd_visible(void);

/* show the digit entry ("" = hide) */
void osd_number(const char *digits);

/* short message at the top (like toast in the HTML5 app): ms = 0 until the next one,
 * NULL/"" = gone; error = red instead of green border */
void osd_message(const char *text, int ms, int error);

/* is the short message currently showing exactly this text? */
int osd_message_is(const char *text);

/* periodically: clock, progress, hiding */
void osd_tick(void);
