/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Teletext and subtitles from the HTSP subscription: a dedicated store with all
 * teletext pages (ttxdec) and the DVB subtitles (dvbsubdec), filled with
 * whatever tvh is currently delivering - live or from the timeshift. The pages
 * survive jumps and reconnects of the same channel, the subtitle events
 * do not (new time base).
 *
 * Queried via fetch_get with "local:ttx/..." (ttx.c, subs.c, hbbweb.c;
 * JSON as formerly with the dvb-buffer server):
 *   local:ttx/pages, local:ttx/<page>[?sub=<hex>], local:ttx/subtitles[?page=&since=],
 *   local:ttx/dvbsub[?pid=<index>&page=<composition>&since=], local:ttx/hbbtv (AIT)
 * The timestamps are those of the HTSP stream - the same as player_stream_pts.
 */
#pragma once

#include "player.h"

/* hooks into the player (feeder thread) and into fetch ("local:") */
void sidedata_init(player *p);

/* channel change (main thread): discard everything, the next subscription rebuilds */
void sidedata_reset(void);

/* current subscription has teletext */
int sidedata_has_teletext(void);

/* teletext start page according to the PMT (hex, e.g. 0x100; tvh with
 * HTSP extension), 0 = unknown */
int sidedata_ttx_initial_page(void);
/* the initial page when HTSP did not name it (from the PMT); 0 = nothing to set */
void sidedata_set_ttx_initial(int page);
