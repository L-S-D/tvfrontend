/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sha1.h"

#include <string.h>

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void block(sha1_ctx *c, const uint8_t *p) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t) p[4 * i] << 24 | (uint32_t) p[4 * i + 1] << 16 | (uint32_t) p[4 * i + 2] << 8 | p[4 * i + 3];
    for (int i = 16; i < 80; i++) w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4], cc = c->h[2];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & cc) | (~b & d);
            k = 0x5A827999;
        } else if (i < 40) {
            f = b ^ cc ^ d;
            k = 0x6ED9EBA1;
        } else if (i < 60) {
            f = (b & cc) | (b & d) | (cc & d);
            k = 0x8F1BBCDC;
        } else {
            f = b ^ cc ^ d;
            k = 0xCA62C1D6;
        }
        uint32_t t = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = cc;
        cc = ROL(b, 30);
        b = a;
        a = t;
    }
    c->h[0] += a;
    c->h[1] += b;
    c->h[2] += cc;
    c->h[3] += d;
    c->h[4] += e;
}

void sha1_init(sha1_ctx *c) {
    static const uint32_t h0[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    memcpy(c->h, h0, sizeof(h0));
    c->len = 0;
    c->n = 0;
}

void sha1_update(sha1_ctx *c, const void *data, size_t len) {
    const uint8_t *p = data;
    c->len += len;
    while (len) {
        size_t k = 64 - c->n < len ? 64 - c->n : len;
        memcpy(c->buf + c->n, p, k);
        c->n += k;
        p += k;
        len -= k;
        if (c->n == 64) {
            block(c, c->buf);
            c->n = 0;
        }
    }
}

void sha1_final(sha1_ctx *c, uint8_t out[20]) {
    uint64_t bits = c->len * 8;
    uint8_t pad = 0x80;
    sha1_update(c, &pad, 1);
    pad = 0;
    while (c->n != 56) sha1_update(c, &pad, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t) (bits >> (56 - 8 * i));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[4 * i] = (uint8_t) (c->h[i] >> 24);
        out[4 * i + 1] = (uint8_t) (c->h[i] >> 16);
        out[4 * i + 2] = (uint8_t) (c->h[i] >> 8);
        out[4 * i + 3] = (uint8_t) c->h[i];
    }
}
