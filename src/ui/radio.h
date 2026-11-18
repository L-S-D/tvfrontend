/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Radio: a channel without a picture gets a screen of its own - logo, name,
 * RadioText and programme on black. The card moves to another place every
 * minute (an OLED must not show the same still picture for hours). Lowest
 * layer: banner, menus and the rest lie above it. Main thread only.
 */
#pragma once

#include "osd.h"

void radio_init(void);

/* the channel, its RadioText (DAB: the dynamic label; NULL/"" = none) and its
 * programme from the guide (now/next NULL = none); unchanged data redraws
 * nothing */
void radio_set(int number, const char *name, const char *icon_url, const char *text, const epg_entry *now,
               const epg_entry *next);

/* show (1) or hide (0) the radio screen */
void radio_show(int on);
int radio_visible(void);

/* periodically: clock, progress, moving the card */
void radio_tick(void);
