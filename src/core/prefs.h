/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Small settings file (JSON) of the app, e.g. the audio track per channel.
 * Lives in the first writable directory out of $HOME, the app directory,
 * /media/developer/temp/com.lsd.tvf (developer mode, when the app folder is
 * owned by root) and /tmp (there only until the TV restarts). Main thread only.
 */
#pragma once

#include "cJSON.h"

void prefs_init(const char *app_dir);

/* entry (owned by prefs, do not free) or NULL */
const cJSON *prefs_get(const char *key);

/* set entry (takes ownership of value; NULL deletes) and write the file */
void prefs_set(const char *key, cJSON *value);
