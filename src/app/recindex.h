/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Time -> byte of a recording directly from the file (tvh's /dvrfile):
 * PCR samples via HTTP Range, linearly interpolated in between.
 * First start and end (duration, usable after two requests), then in the
 * background ever finer (coarse to fine, up to INDEX_ANCHORS points). Each
 * sample reads only up to the first PCR (a few KB) and never ends at the
 * end of the file - tvh counts a response up to the end as "played".
 */
#pragma once

typedef struct recindex recindex;

/* url: file (http://user:pass@host:port/dvrfile/<uuid>) */
recindex *recindex_fetch(const char *url);

/* 0 = still loading, 1 = usable (still getting finer), -1 = failed */
int recindex_state(recindex *x);

/* duration according to PCR (start to end of the file at load time), 0 = unknown */
double recindex_duration(recindex *x);

/* byte offset (packet boundary) for time t and the time at that point. 0 = ok */
int recindex_lookup(recindex *x, double t, long long *offset, double *seg_t);

/* may also be called while loading */
void recindex_free(recindex *x);
