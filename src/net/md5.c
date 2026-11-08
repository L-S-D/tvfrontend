/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "md5.h"

#include <stdio.h>
#include <string.h>

#define ROTL(x, c) (((x) << (c)) | ((x) >> (32 - (c))))

static const uint32_t K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static void md5_block(md5_ctx *c, const uint8_t *p) {
    uint32_t w[16];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t) p[i * 4] | ((uint32_t) p[i * 4 + 1] << 8) |
               ((uint32_t) p[i * 4 + 2] << 16) | ((uint32_t) p[i * 4 + 3] << 24);
    }
    uint32_t a = c->state[0], b = c->state[1], cc = c->state[2], d = c->state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & cc) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & cc);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ cc ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = cc ^ (b | ~d);
            g = (7 * i) & 15;
        }
        uint32_t tmp = d;
        d = cc;
        cc = b;
        b = b + ROTL(a + f + K[i] + w[g], R[i]);
        a = tmp;
    }
    c->state[0] += a;
    c->state[1] += b;
    c->state[2] += cc;
    c->state[3] += d;
}

void md5_init(md5_ctx *c) {
    c->state[0] = 0x67452301;
    c->state[1] = 0xefcdab89;
    c->state[2] = 0x98badcfe;
    c->state[3] = 0x10325476;
    c->bytes = 0;
    c->fill = 0;
}

void md5_update(md5_ctx *c, const void *data, size_t len) {
    const uint8_t *p = data;
    c->bytes += len;
    while (len > 0) {
        size_t n = 64 - c->fill;
        if (n > len) {
            n = len;
        }
        memcpy(c->block + c->fill, p, n);
        c->fill += n;
        p += n;
        len -= n;
        if (c->fill == 64) {
            md5_block(c, c->block);
            c->fill = 0;
        }
    }
}

void md5_final(md5_ctx *c, uint8_t out[16]) {
    uint64_t bits = c->bytes * 8;
    static const uint8_t pad = 0x80;
    static const uint8_t zero[64] = {0};
    md5_update(c, &pad, 1);
    size_t z = (c->fill <= 56) ? 56 - c->fill : 120 - c->fill;
    md5_update(c, zero, z);
    uint8_t lenle[8];
    for (int i = 0; i < 8; i++) {
        lenle[i] = (uint8_t) (bits >> (8 * i));
    }
    md5_update(c, lenle, 8);
    for (int i = 0; i < 4; i++) {
        out[i * 4] = (uint8_t) c->state[i];
        out[i * 4 + 1] = (uint8_t) (c->state[i] >> 8);
        out[i * 4 + 2] = (uint8_t) (c->state[i] >> 16);
        out[i * 4 + 3] = (uint8_t) (c->state[i] >> 24);
    }
}

void md5_hex(const char *s, char out[33]) {
    md5_ctx c;
    uint8_t d[16];
    md5_init(&c);
    md5_update(&c, s, strlen(s));
    md5_final(&c, d);
    for (int i = 0; i < 16; i++) {
        snprintf(out + i * 2, 3, "%02x", d[i]);
    }
}
