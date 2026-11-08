/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * HLS with MPEG-TS segments as a continuous TS stream for the player: the
 * segments are fetched one after another and delivered back to back, the
 * existing TS demuxer notices no difference. Live: start three segments
 * before the end, the playlist is reloaded continuously. VOD: start in the segment
 * that contains the requested time.
 *
 * Not supported (error on open): fMP4 segments (EXT-X-MAP),
 * encryption (EXT-X-KEY other than NONE), separate audio tracks as their own
 * playlist without a variant with embedded audio.
 */
#pragma once

#include "http.h"

#include <stddef.h>
#include <sys/types.h>

typedef struct hls hls;

/* url: master or media playlist; start: seconds (VOD). hook as with
 * http_open_ex (cancel from the main thread), stop: cancel flag. NULL on
 * error (reason in err). */
hls *hls_open(const char *url, double start, const http_hook *hook, volatile int *stop, char *err, size_t errlen);

/* > 0 bytes, 0 = end (VOD), < 0 error */
ssize_t hls_read(hls *h, void *buf, size_t n);

double hls_duration(const hls *h); /* VOD: total duration, live: 0 */
double hls_start_pos(const hls *h); /* time at the start of the first delivered segment */
int hls_is_live(const hls *h);

void hls_close(hls *h);
