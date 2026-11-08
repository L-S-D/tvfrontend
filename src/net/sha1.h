/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * SHA-1 (RFC 3174) - only for HTSP authentication:
 * digest = SHA1(password + 32-byte challenge from the hello reply).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    size_t n;
} sha1_ctx;

void sha1_init(sha1_ctx *c);
void sha1_update(sha1_ctx *c, const void *data, size_t len);
void sha1_final(sha1_ctx *c, uint8_t out[20]);
