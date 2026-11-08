/* SPDX-License-Identifier: GPL-3.0-or-later */
/* MD5 (RFC 1321) - only for HTTP digest auth against TVHeadend. */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t state[4];
    uint64_t bytes;
    uint8_t block[64];
    size_t fill;
} md5_ctx;

void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, size_t len);
void md5_final(md5_ctx *c, uint8_t out[16]);

/* MD5 as 32 lowercase hex digits plus a NUL byte. */
void md5_hex(const char *s, char out[33]);
