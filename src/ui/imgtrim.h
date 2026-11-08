/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Channel logos: cut away a uniformly coloured or transparent border (like
 * sharp trim). Thread-safe (stb, without LVGL),
 * for the fetch threads of the user interface.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* PNG in, trimmed PNG in *out (malloc, to be freed by the caller).
 * 1 = trimmed, 0 = not a PNG, not readable or nothing to cut away. */
int img_trim_png(const uint8_t *in, size_t len, uint8_t **out, size_t *out_len);

/* JPEG (progressive too) -> pixels for LVGL (ARGB8888, byte order B,G,R,A) in
 * *out (malloc), scaled down by an integer factor to at most max_w x max_h.
 * 1 = ok, 0 = not a JPEG or not readable. */
int img_decode_jpeg(const uint8_t *in, size_t len, int max_w, int max_h, uint8_t **out, int *w, int *h);
