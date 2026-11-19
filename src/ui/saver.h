/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Screen protection for an OLED: over a still picture (radio, a paused
 * picture, an open menu, no picture) after a while without a key. Stage 1
 * dims everything, the video plane too, to 40 %; stage 2 makes the screen
 * black (an OLED pixel showing black is off) - the app switches the panel
 * off then, where the TV lets it. The topmost layer.
 * When to go to which stage is the app's (main.c); main thread only.
 */
#pragma once

void saver_init(void); /* after all other layers */

/* 0 off, 1 dimmed, 2 dark */
void saver_set(int stage);
int saver_stage(void);

/* periodically: stays the topmost layer */
void saver_tick(void);
