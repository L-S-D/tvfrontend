/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Teletext page store (EN 300 472 / ETS 300 706 Level 1), C version of the
 * TeletextCache from dvb-buffer: fed with the teletext data that tvh is
 * currently delivering (HTSP stream TELETEXT, live or from timeshift),
 * it holds every page with subpages, FLOF links and national character
 * set and returns them ready to draw: characters (mosaic as
 * U+E000 + sextant mask), colours, flash, conceal, double height.
 * Plus the subtitles (pages with C6): every transmission with new content is
 * an event with the rows in the box and the PTS from which it applies.
 *
 * The JSON responses have the format of the dvb-buffer server (/ttx/...), so
 * ttx.c and subs.c stay unchanged. Fed from one thread, the
 * queries from another (own lock).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct ttxdec ttxdec;

ttxdec *ttxdec_new(void);
void ttxdec_free(ttxdec *t);

/* payload of a PES from data_identifier (that is how tvh delivers TELETEXT over HTSP),
 * pts in 90 kHz (-1 = none) */
void ttxdec_feed(ttxdec *t, const uint8_t *d, size_t len, int64_t pts);
/* gap in the input (jump in timeshift): discard partly received pages, the
 * store stays */
void ttxdec_reset_input(ttxdec *t);
/* discard subtitle events (new time base); the pages stay */
void ttxdec_clear_events(ttxdec *t);

int ttxdec_page_count(ttxdec *t);

/* JSON (malloc, owned by the caller):
 *   pages: {"pages":["100","101",...]}
 *   page:  {"page":"100","sub":"0001","subs":[...],"national":4,"age_ms":800,
 *           "links":[6 pages or null]|null,"fastext":true,"rows":[25 x
 *           {"t","fg","bg","a"[,"hide"]}]} - NULL = page not there;
 *           sub = subpage code, -1 = last received
 *   subtitles: {"page":"150","epoch":E,"events":[{"seq","pts","lines":[{"t","c","r"}]}],"next":N}
 * epoch changes with ttxdec_clear_events (the client then discards its
 * events); seq and epoch are unique across all decoders */
char *ttxdec_pages_json(ttxdec *t);
char *ttxdec_page_json(ttxdec *t, int page, int sub);
char *ttxdec_subtitles_json(ttxdec *t, int page, uint64_t since);

/* rows 0..24 as text (mosaic as spaces), for tests: n rows of 160 bytes each */
int ttxdec_page_text(ttxdec *t, int page, int sub, char rows[25][160]);
