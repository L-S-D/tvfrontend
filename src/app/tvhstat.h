/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Reception info for the current channel from tvh for the banner: from the
 * status messages of the player's live subscription (player_set_htsp_status) -
 * sourceinfo at start (provider, adapter, mux, network), signalStatus
 * (ongoing: status, SNR, signal, BER, UNC) and descrambleInfo (CAID,
 * card system, reader, ECM time).
 */
#pragma once

#include "cJSON.h"

#include <stdint.h>

typedef struct {
    uint32_t channel_id; /* HTSP channel, 0 = show nothing */
    int started;         /* subscriptionStart arrived */
    char provider[64], service[96], adapter[96], mux[64], network[64];
    /* signalStatus: values only valid with has_* */
    char status[16];     /* feStatus: GOOD, BAD, FAINT, NONE (empty = none) */
    int has_snr, snr_abs; /* abs: snr in 0.001 dB, otherwise 0..65535 */
    int64_t snr;
    int has_signal, signal_abs; /* abs: signal in 0.001 dBm, otherwise 0..65535 */
    int64_t signal;
    int has_ber, has_unc;
    uint32_t ber, unc;
    /* descrambleInfo */
    int has_descr;
    uint32_t caid, provid, ecmtime, hops;
    char cardsystem[32], reader[64], from[64], protocol[32];
    char error[96]; /* subscriptionStop/-Status, "" = none */
    /* tvheadend fork (earlyStart/reportNoAccess): the channel is scrambled and
     * its key does not come, and the CA systems of its CA streams */
    int no_access;
    char ca_list[96]; /* "Viaccess 0500, 4AFC", "" = unknown */
} tvhstat_info;

/* status message of the live subscription on channel channel (player thread) */
void tvhstat_feed(unsigned long channel, const char *method, const cJSON *msg);

/* channel whose reception should be shown (HTSP channel ID), 0 = none */
void tvhstat_watch(uint32_t channel_id);

/* fetch the current state (only for the watched channel, otherwise channel_id 0):
 * 1 = changed since the last call */
int tvhstat_get(tvhstat_info *out);

/* the subscription of channel channel_id reports "No access" (also before its
 * first picture, without watching it); out = its state. 1 = yes */
int tvhstat_no_access(uint32_t channel_id, tvhstat_info *out);
