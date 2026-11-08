/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Rewrite the entry point of HbbTV apps before TVF loads the address from the
 * AIT (res/hbbtv/entry.json, applied in hbbweb.c): for entry pages that
 * detect the device by its User-Agent and then do not pick the matching
 * player. Main thread only.
 */
#pragma once

/* Load entry.json: {"rewrites": [[regex, replacement], ...]}. 0 = loaded */
int hbbtv_load(const char *path);

/* Rewrites as a JSON array or NULL (none) */
const char *hbbtv_rewrites(void);
