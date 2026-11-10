/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Reception of the current channel below the logo (tvh: provider, bars for SNR and
 * signal in the colour of the quality, the decryption as a framed box with ECM time
 * and reader below). The banner shows it compactly; the info window in detail with a
 * quality line (status, BER, UNC) and the DVB ids of the channel. Main thread only.
 */
#pragma once

#include "lvgl.h"

/* reception, preformatted (main.c) */
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
    char quality[64];     /* detail: "Good · BER 0 · UNC 0", "" = none */
} osd_rx;

typedef struct rxview rxview;

/* at x/y of parent, 222 px wide; detail: quality line and DVB ids (info window) */
rxview *rxview_create(lv_obj_t *parent, int x, int y, int detail);
/* r NULL = no reception; ids "" = none (detail only) */
void rxview_set(rxview *v, const osd_rx *r, const char *ids);
void rxview_show(rxview *v, int on);
