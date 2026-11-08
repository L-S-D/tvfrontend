/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * DVB subtitle decoder (EN 300 743), C version of the DvbSubDecoder from
 * dvb-buffer, for one subtitle stream and one composition page: page and
 * region composition, CLUTs (with the defaults), pixel data (2/4/8 bit
 * run length, top/bottom field), display definition. Every display set
 * becomes an event with the PTS of its PES and the visible regions
 * (position, palette, pixels run-length coded) - no regions = off.
 *
 * Fed from one thread, queried from another (own lock).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct dvbsubdec dvbsubdec;

dvbsubdec *dvbsubdec_new(int composition_page, int ancillary_page);
void dvbsubdec_free(dvbsubdec *d);

/* segments of a PES (from the first 0x0F, without data_identifier and
 * stream_id - that is how tvh delivers DVBSUB over HTSP), pts in 90 kHz */
void dvbsubdec_feed(dvbsubdec *d, const uint8_t *seg, size_t len, int64_t pts);
/* new time base / gap: discard epoch and events */
void dvbsubdec_reset(dvbsubdec *d);

/* {"page":1,"epoch":E,"events":[{"seq","pts","timeout","w","h","regions":[{"x","y","w","h",
 *   "pal":"rrggbbaa..","rle":"<base64>"}]}],"next":N} (malloc)
 * rle: pairs (run length - 1, palette index) across the rows */
char *dvbsubdec_events_json(dvbsubdec *d, uint64_t since);
size_t dvbsubdec_display_sets(dvbsubdec *d);
