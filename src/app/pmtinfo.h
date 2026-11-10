/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * What a tvh without the HTSP extensions leaves out of the stream description, from the PMT of the
 * channel: subtitle pages of the teletext (descriptor type 2 = subtitles, 5 = for the hearing
 * impaired; HTSP names neither them nor a shared teletext's programme), component tags, the
 * subtitling type of DVB subtitles, the initial teletext page, audio description and receiver mix.
 * Once per channel, briefly open the HTTP stream with the pass profile (same service as the HTSP
 * subscription, no second tuner), read PAT/PMT with the stream demuxer, then the SDT for the DVB ids
 * (HbbTV; a tvh with the extension sends them in subscriptionStart), close. The result is kept
 * until restart. The merge into the stream list is player_merge_pmt().
 */
#pragma once

#include "ts.h"

#include <stddef.h>

/* Build option TVF_PMT_FALLBACK (CMake, default ON): without it these are empty functions */
#ifndef TVF_PMT_FALLBACK
#define TVF_PMT_FALLBACK 1
#endif

typedef struct {
    int page;     /* hex, e.g. 0x777 */
    int type;     /* 2 = subtitles, 5 = for the hearing impaired */
    char lang[4]; /* ISO 639, e.g. "deu" */
} pmtinfo_page;

/* called (from the lookup thread, or at once for a known channel) with the PMT of the channel */
typedef void (*pmtinfo_cb)(unsigned long channel, const ts_pmt *pmt);

#if TVF_PMT_FALLBACK
void pmtinfo_init(const char *host, int http_port, const char *user, const char *pass);
void pmtinfo_set_callback(pmtinfo_cb cb);

/* start a lookup in the background (known: the callback again; in progress: nothing) */
void pmtinfo_lookup(unsigned long channel);

/* subtitle pages into out; -1 = unknown (so far) */
int pmtinfo_pages(unsigned long channel, pmtinfo_page *out, int max);
#else
static inline void pmtinfo_init(const char *host, int http_port, const char *user, const char *pass) {
    (void) host, (void) http_port, (void) user, (void) pass;
}
static inline void pmtinfo_set_callback(pmtinfo_cb cb) { (void) cb; }
static inline void pmtinfo_lookup(unsigned long channel) { (void) channel; }
static inline int pmtinfo_pages(unsigned long channel, pmtinfo_page *out, int max) {
    (void) channel, (void) out, (void) max;
    return -1;
}
#endif

