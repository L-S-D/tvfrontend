/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Shared parts of the LVGL user interface: fonts, images (via HTTP, with
 * cache), colours and small building blocks. Without SDL - the display itself is in
 * ui_sdl.c (TV) or in the PC test. Everything here only from the main thread.
 */
#pragma once

#include "lvgl.h"

#include <stddef.h>

/* colours (like the HTML5 app) */
#define UI_BLUE 0x2196F3
#define UI_RED 0xF44336
#define UI_GREEN 0x4CAF50
#define UI_YELLOW 0xFFEB3B
#define UI_HINT_BLUE 0x42A5F5
#define UI_TEXT 0xF2F4F7
#define UI_DIM 0xA7B0BE
#define UI_PANEL 0x0E1116
#define UI_BAR_BG 0x39404C

/* res_dir: directory with fonts/; wake: wakes the main thread when an
 * image has loaded (call ui_poll afterwards). */
void ui_res_init(const char *res_dir, void (*wake)(void));

/* Lato at px size (regular/bold), cached; missing characters
 * (LV_SYMBOL_*) come from Montserrat. */
const lv_font_t *ui_font(int px, int bold);

/* Fetch an image (PNG/JPEG) asynchronously via HTTP. cb is called in the main thread from
 * ui_poll(), dsc stays valid in the cache (NULL on error). Identical URLs
 * are loaded only once. */
typedef void (*ui_image_cb)(void *user, const char *url, const lv_image_dsc_t *dsc);
void ui_image_fetch(const char *url, ui_image_cb cb, void *user);
/* like ui_image_fetch, but only the last few stay in the cache (large
 * EPG images): whoever shows such an image must switch to another one
 * before the next fetch */
void ui_image_fetch_transient(const char *url, ui_image_cb cb, void *user);
/* load images whose URL starts with url_prefix (tvh web server) with this login
 * (Digest/Basic after 401); set before the first fetch */
void ui_image_login(const char *url_prefix, const char *user, const char *pass);
void ui_poll(void);

/* building blocks */
lv_obj_t *ui_box(lv_obj_t *parent, uint32_t color, lv_opa_t opa, int radius);
lv_obj_t *ui_label(lv_obj_t *parent, int px, int bold, uint32_t color);
void ui_fmt_hm(long t, char *out, size_t n); /* Unix seconds -> "HH:MM" local time */
/* (x, y) within the object's screen coordinates? (visible objects) */
int ui_hit(lv_obj_t *obj, int x, int y);

/* label on one line with "..." at the end (height = one line, set width beforehand) */
void ui_label_dots(lv_obj_t *label);

/* text for single-line display: line breaks/tabs -> spaces, without
 * spaces at start/end, shortened to max bytes (at a UTF-8 boundary). */
/* short weekday in the user interface language (tm_wday) */
const char *ui_weekday(int wday);
void ui_oneline(char *out, size_t n, const char *in, size_t max);
