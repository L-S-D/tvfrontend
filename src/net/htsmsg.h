/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * HTSP messages (htsmsg, TVHeadend's binary format): building and reading.
 * On the wire: 4-byte length (big-endian), then fields of type (1 byte),
 * name length (1), data length (4, big-endian), name and data. Numbers
 * (S64) little-endian with as few bytes as needed, maps and lists
 * nested. Parsed into cJSON: numbers as double (IDs and
 * times fit exactly), BIN and UUID as hex text.
 */
#pragma once

#include "cJSON.h"

#include <stddef.h>
#include <stdint.h>

/* build a flat message (requests need no nesting) */
typedef struct {
    unsigned char *p;
    size_t len, cap;
    int err;
} hmsg;

void hmsg_init(hmsg *m);
void hmsg_free(hmsg *m);
void hmsg_s64(hmsg *m, const char *name, int64_t v);
void hmsg_str(hmsg *m, const char *name, const char *s);
void hmsg_list_s64(hmsg *m, const char *name, const int64_t *v, size_t n); /* list of numbers */
void hmsg_bin(hmsg *m, const char *name, const void *data, size_t n);

/* finished message including the length field; NULL on allocation failure. Owned by m. */
const unsigned char *hmsg_wire(hmsg *m, size_t *len);

/* body of a message (without the 4-byte length) -> cJSON object, NULL = corrupt */
cJSON *hmsg_parse(const unsigned char *buf, size_t len);

/* hex text (from hmsg_parse) back to bytes; count, -1 = invalid */
int hmsg_unhex(const char *hex, unsigned char *out, size_t max);

/* Read the fields of a message one by one, without cJSON (for the many
 * muxpkt of a subscription). type as on the wire (2 = S64, 3 = STR, 4 = BIN ...),
 * name is not NUL-terminated. */
typedef struct {
    int type;
    const char *name;
    size_t nlen;
    const unsigned char *data;
    size_t dlen;
} hmsg_field;

/* next field from *pos: 1 = read, 0 = end, -1 = corrupt */
int hmsg_next(const unsigned char *buf, size_t len, size_t *pos, hmsg_field *f);
/* compare the name */
int hmsg_is(const hmsg_field *f, const char *name);
/* S64 value (other types: 0) */
int64_t hmsg_s64_of(const hmsg_field *f);
