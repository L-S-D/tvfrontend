/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "imgtrim.h"

#include <stdlib.h>
#include <string.h>

/* stb as a static copy: gcc reports unused functions only at the end of the file */
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG /* EPG images: progressive too (LVGL's TJPGD only baseline) */
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#pragma GCC diagnostic pop

#define TRIM_TOL 10 /* deviation per channel that still counts as border (sharp: 10) */

int img_trim_png(const uint8_t *in, size_t len, uint8_t **out, size_t *out_len) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    *out = NULL;
    *out_len = 0;
    if (!in || len < 8 || len > 16u << 20 || memcmp(in, sig, 8) != 0) return 0;
    int w = 0, h = 0, n = 0;
    uint8_t *px = stbi_load_from_memory(in, (int) len, &w, &h, &n, 4);
    if (!px) return 0;
    /* reference: top left pixel; if it is transparent, only alpha counts */
    const uint8_t bg[4] = {px[0], px[1], px[2], px[3]};
    int clear = bg[3] <= TRIM_TOL;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int y = 0; y < h; y++) {
        const uint8_t *row = px + (size_t) y * (size_t) w * 4;
        for (int x = 0; x < w; x++) {
            const uint8_t *q = row + (size_t) x * 4;
            int edge = clear ? q[3] <= TRIM_TOL
                             : abs(q[0] - bg[0]) <= TRIM_TOL && abs(q[1] - bg[1]) <= TRIM_TOL &&
                                   abs(q[2] - bg[2]) <= TRIM_TOL && abs(q[3] - bg[3]) <= TRIM_TOL;
            if (edge) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    int ok = 0;
    if (x1 >= x0 && y1 >= y0 && (x0 > 0 || y0 > 0 || x1 < w - 1 || y1 < h - 1)) {
        int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
        const uint8_t *start = px + ((size_t) y0 * (size_t) w + (size_t) x0) * 4;
        int plen = 0;
        unsigned char *png = stbi_write_png_to_mem(start, w * 4, cw, ch, 4, &plen);
        if (png && plen > 0) {
            *out = png; /* STBIW_MALLOC = malloc */
            *out_len = (size_t) plen;
            ok = 1;
        } else {
            free(png);
        }
    }
    stbi_image_free(px);
    return ok;
}

int img_decode_jpeg(const uint8_t *in, size_t len, int max_w, int max_h, uint8_t **out, int *w, int *h) {
    *out = NULL;
    *w = *h = 0;
    if (!in || len < 4 || len > 16u << 20 || in[0] != 0xFF || in[1] != 0xD8 || in[2] != 0xFF) return 0;
    int iw = 0, ih = 0, n = 0;
    uint8_t *px = stbi_load_from_memory(in, (int) len, &iw, &ih, &n, 4);
    if (!px) return 0;
    /* scale down by an integer factor (average over f x f) until it fits into max_w x max_h */
    int f = 1;
    while (iw / f > max_w || ih / f > max_h) f++;
    int ow = iw / f, oh = ih / f;
    uint8_t *o = ow > 0 && oh > 0 ? malloc((size_t) ow * (size_t) oh * 4) : NULL;
    if (!o) {
        stbi_image_free(px);
        return 0;
    }
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++) {
            unsigned r = 0, g = 0, b = 0;
            for (int dy = 0; dy < f; dy++) {
                const uint8_t *p = px + ((size_t) (y * f + dy) * (size_t) iw + (size_t) x * f) * 4;
                for (int dx = 0; dx < f; dx++, p += 4) {
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            }
            unsigned d = (unsigned) (f * f);
            uint8_t *q = o + ((size_t) y * (size_t) ow + (size_t) x) * 4;
            q[0] = (uint8_t) (b / d); /* LVGL ARGB8888 in memory: B, G, R, A */
            q[1] = (uint8_t) (g / d);
            q[2] = (uint8_t) (r / d);
            q[3] = 0xFF;
        }
    stbi_image_free(px);
    *out = o;
    *w = ow;
    *h = oh;
    return 1;
}
