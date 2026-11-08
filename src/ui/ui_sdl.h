/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * LVGL on the webOS SDL window.
 *
 * The window lies above the video plane: whatever LVGL does not draw stays
 * transparent (alpha 0) and shows the video. LVGL renders directly into an
 * ARGB8888 buffer of screen size; changed areas go into an
 * SDL texture that is drawn without blending (alpha is copied).
 */
#pragma once

#include <SDL.h>

#include "ui.h"

/* SDL event type: an image download has finished (call ui_poll). */
extern uint32_t ui_sdl_event;

int ui_init(SDL_Renderer *ren, int w, int h, const char *res_dir);
void ui_shutdown(void);

/* user interface layer as PNG (RGBA, without video) via TCP to "ip:port" -
 * development aid. 0 = ok */
int ui_screenshot_send(const char *hostport);

/* Let LVGL do its work; returns ms until it is next needed. */
uint32_t ui_frame(void);

/* layer below the user interface (HbbTV page: ARGB8888, premultiplied alpha,
 * scaled to the whole window), NULL = none. With a layer LVGL blends
 * on top instead of copying its alpha. */
void ui_set_layer(SDL_Texture *t);

/* recompose the window (layer + LVGL) and show it - after a new frame
 * of the layer */
void ui_compose(void);

SDL_Renderer *ui_renderer(void);
