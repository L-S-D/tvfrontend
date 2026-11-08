/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Playback of a recording: the entry comes from tvrec (fileUrl =
 * tvh's /dvrfile, position = resume). The file is played directly;
 * recindex provides the byte position for jumps from PCR samples of the
 * file. Positions go to tvrec_position every 10 s and on pause/end
 * (-> tvh).
 * Main thread only.
 */
#pragma once

#include "cJSON.h"
#include "player.h"

#include <stdint.h>

typedef struct {
    /* playback left (Back/Stop/end): id of the recording */
    void (*exited)(const char *id);
} recplay_actions;

void recplay_init(player *pl, const recplay_actions *a);

void recplay_start(const cJSON *msg);
int recplay_active(void);
/* current recording (Kore): HTSP ID, position/duration (s), pause. 0 = none */
int recplay_info(uint32_t *num, double *pos, double *dur, int *paused);

/* report the position (stopped) and end playback; does not call exited */
void recplay_exit(void);

/* tvh reconnected during playback: report the state */
void recplay_report(void);

/* playerControl (Kore): pause / play / seek */
void recplay_control(const cJSON *msg);

/* switch audio track: restart at the current position */
void recplay_select_audio(int pid);

/* Pointer (ptr_event) like the HTML5 app: moving brings up the controls, above the
 * bar the target time is shown, click jumps, dragging (OK held) scrubs;
 * click elsewhere: show controls or pause. 1 = consumed */
int recplay_pointer(int ev, int x, int y);
/* pointer hidden: discard dragging */
void recplay_pointer_gone(void);

/* wheel: dir > 0 = 30 s forward, otherwise 30 s back */
void recplay_wheel(int dir);

/* app_key; 1 = consumed */
int recplay_key(int key);

/* app to background/foreground: release the decoder or continue at the position */
void recplay_suspend(void);
void recplay_resume(void);

void recplay_tick(void);
