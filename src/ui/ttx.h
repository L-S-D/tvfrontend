/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Teletext (Blue) like ttx in the HTML5 app: the pages come ready
 * prepared from the HTSP subscription's own page store (sidedata.c,
 * "local:ttx/<page>": 25 rows x 40
 * cells with characters, colours, mosaic as U+E000+mask, attributes) - immediately,
 * without waiting for the carousel cycle. Drawn with Bedstead (SAA5050 replica),
 * each cell 36 x 42 pixels. Main thread only.
 */
#pragma once

typedef struct {
    void (*zap)(int dir); /* CH+/- in teletext: close and switch */
    /* start page of the channel (hex, e.g. 0x100), 0 = unknown (then 100);
     * applies when first opened on a channel */
    int (*initial_page)(void);
} ttx_actions;

void ttx_init(const char *res_dir, const ttx_actions *a);

/* channel change: local 1 = page store of the HTSP subscription (sidedata.c), 0 =
 * channel without teletext (IPTV). Closes an open view. */
void ttx_set_source(int local);
int ttx_available(void);

void ttx_open(void);
void ttx_close(void);
int ttx_visible(void);

/* app_key; 1 = consumed (open: all keys) */
int ttx_key(int key);

/* reload page (1 s), flash (0.5 s) */
void ttx_tick(void);
