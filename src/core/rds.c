/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* IEC 62106, annex E (the RDS G0 code table), 0x80..0xFE as Unicode */
static const uint16_t g0_high[127] = {
    0x00E1, 0x00E0, 0x00E9, 0x00E8, 0x00ED, 0x00EC, 0x00F3, 0x00F2, 0x00FA, 0x00F9, 0x00D1, 0x00C7, 0x015E, 0x00DF,
    0x00A1, 0x0132, 0x00E2, 0x00E4, 0x00EA, 0x00EB, 0x00EE, 0x00EF, 0x00F4, 0x00F6, 0x00FB, 0x00FC, 0x00F1, 0x00E7,
    0x015F, 0x01E7, 0x0131, 0x0133, 0x00AA, 0x03B1, 0x00A9, 0x2030, 0x01E6, 0x011B, 0x0148, 0x0151, 0x03C0, 0x20AC,
    0x00A3, 0x0024, 0x2190, 0x2191, 0x2192, 0x2193, 0x00BA, 0x00B9, 0x00B2, 0x00B3, 0x00B1, 0x0130, 0x0144, 0x0171,
    0x00B5, 0x00BF, 0x00F7, 0x00B0, 0x00BC, 0x00BD, 0x00BE, 0x00A7, 0x00C1, 0x00C0, 0x00C9, 0x00C8, 0x00CD, 0x00CC,
    0x00D3, 0x00D2, 0x00DA, 0x00D9, 0x0158, 0x010C, 0x0160, 0x017D, 0x00D0, 0x013F, 0x00C2, 0x00C4, 0x00CA, 0x00CB,
    0x00CE, 0x00CF, 0x00D4, 0x00D6, 0x00DB, 0x00DC, 0x0159, 0x010D, 0x0161, 0x017E, 0x0111, 0x0140, 0x00C3, 0x00C5,
    0x00C6, 0x0152, 0x0177, 0x00DD, 0x00D5, 0x00D8, 0x00DE, 0x014A, 0x0154, 0x0106, 0x015A, 0x0179, 0x0166, 0x00F0,
    0x00E3, 0x00E5, 0x00E6, 0x0153, 0x0175, 0x00FD, 0x00F5, 0x00F8, 0x00FE, 0x014B, 0x0155, 0x0107, 0x015B, 0x017A,
    0x0167};

static uint16_t g0(uint8_t c) {
    switch (c) {
    case 0x24: return 0x00A4;
    case 0x5E: return 0x2015;
    case 0x60: return 0x2016;
    case 0x7E: return 0x00AF;
    case 0x7F: return 0;
    case 0xFF: return 0;
    default: return c >= 0x80 ? g0_high[c - 0x80] : c >= 0x20 ? c : 0;
    }
}

/* CRC of a UECP frame: CCITT, preset to ones, inverted */
static uint16_t crc16(const uint8_t *d, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t) (d[i] << 8);
        for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t) ((crc << 1) ^ 0x1021) : (uint16_t) (crc << 1);
    }
    return (uint16_t) ~crc;
}

/* text in the RDS character set to UTF-8 */
static void to_utf8(const uint8_t *t, size_t len, char *out, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint16_t cp = g0(t[i]);
        if (!cp) continue;
        char u[3];
        size_t k;
        if (cp < 0x80) u[0] = (char) cp, k = 1;
        else if (cp < 0x800) u[0] = (char) (0xC0 | cp >> 6), u[1] = (char) (0x80 | (cp & 0x3F)), k = 2;
        else u[0] = (char) (0xE0 | cp >> 12), u[1] = (char) (0x80 | ((cp >> 6) & 0x3F)), u[2] = (char) (0x80 | (cp & 0x3F)), k = 3;
        if (o + k >= n) break;
        memcpy(out + o, u, k);
        o += k;
    }
    while (o && out[o - 1] == ' ') o--;
    out[o] = 0;
}

#define UECP_MAX 300 /* a frame without its stuffing: at most 4 + 255 + 2 bytes */

/* One frame between 0xFE and 0xFF (both left out), still stuffed, into u
 * (UECP_MAX bytes): its message field, and the length of that into *mfl.
 * NULL = not a frame with a good CRC. */
static const uint8_t *message(const uint8_t *s, size_t len, uint8_t *u, size_t *mfl) {
    /* unstuffing: FD 00 / FD 01 / FD 02 stand for FD / FE / FF */
    size_t m = 0;
    for (size_t i = 0; i < len && m < UECP_MAX; i++) {
        if (s[i] == 0xFD && i + 1 < len && s[i + 1] <= 2) u[m++] = (uint8_t) (0xFD + s[++i]);
        else u[m++] = s[i];
    }
    /* address (2), sequence counter, message field length, message field, CRC (2) */
    if (m < 6 || (size_t) u[3] + 6 != m) return NULL;
    if (crc16(u, m - 2) != (uint16_t) (u[m - 2] << 8 | u[m - 1])) return NULL;
    *mfl = u[3];
    return u + 4;
}

/* one frame between 0xFE and 0xFF (both left out), still stuffed: 1 = a
 * RadioText, 2 = the whole dynamic label, 0 = neither */
static int frame(const uint8_t *s, size_t len, char *out, size_t n) {
    uint8_t u[UECP_MAX];
    size_t mfl = 0;
    const uint8_t *msg = message(s, len, u, &mfl);
    if (!msg) return 0;
    /* The whole dynamic label of DAB as it is (libdvbdab sends it after the
     * RadioText): a manufacturer's specific command, MEC 0x2D, length, the
     * designation "dl", the command 0x01, the text in UTF-8. RadioText has
     * 64 characters; a longer label comes there in parts that take turns. */
    if (mfl >= 5 && msg[0] == 0x2D && msg[1] >= 3 && (size_t) msg[1] + 2 <= mfl && msg[2] == 'd' && msg[3] == 'l' &&
        msg[4] == 0x01) {
        size_t tl = (size_t) msg[1] - 3;
        if (n == 0) return 0;
        if (tl > n - 1) {
            tl = n - 1;
            while (tl && ((uint8_t) msg[5 + tl] & 0xC0) == 0x80) tl--; /* not inside a character */
        }
        memcpy(out, msg + 5, tl);
        while (tl && out[tl - 1] == ' ') tl--;
        out[tl] = 0;
        return 2;
    }
    /* RadioText: MEC 0x0A, data set, programme service, length, configuration, text */
    if (mfl < 5 || msg[0] != 0x0A || (size_t) msg[3] + 4 > mfl || msg[3] < 1) return 0;
    to_utf8(msg + 5, (size_t) msg[3] - 1, out, n);
    return 1;
}

int rds_uecp_radiotext(const uint8_t *d, size_t len, char *out, size_t n) {
    int found = 0;
    char t[256];
    for (size_t i = 0; i < len; i++) {
        if (d[i] != 0xFE) continue;
        size_t j = i + 1;
        while (j < len && d[j] != 0xFF) j++;
        if (j >= len) break;
        int what = frame(d + i + 1, j - i - 1, t, sizeof(t));
        /* the whole label comes before a RadioText, which may be a part of it */
        if (what >= found && what) {
            snprintf(out, n, "%s", t);
            found = what;
        }
        i = j;
    }
    return found != 0;
}

/* ---------------- the slideshow ---------------- */

#define SLIDE_PIECE 240
#define SLIDE_PIECES 600 /* more than libdvbdab sends (128 KiB) */

void rds_slide_init(rds_slide *s) {
    memset(s, 0, sizeof(*s));
    s->id = s->done = -1;
}

void rds_slide_free(rds_slide *s) {
    free(s->got);
    free(s->buf);
    rds_slide_init(s);
}

/* a piece: slide, piece, pieces, the bytes. 1 = the slide is complete */
static int slide_piece(rds_slide *s, int id, unsigned piece, unsigned count, const uint8_t *b, size_t n) {
    if (count == 0 || count > SLIDE_PIECES || piece >= count || n == 0 || n > SLIDE_PIECE) return 0;
    if (piece + 1 < count && n != SLIDE_PIECE) return 0; /* only the last one is shorter */
    if (id == s->done) return 0;                         /* that one again, for those who came later */
    if (id != s->id || count != s->count) {
        /* another slide begins: what was collected of the last one is of no use */
        uint8_t *got = calloc(count, 1), *buf = malloc((size_t) count * SLIDE_PIECE);
        if (!got || !buf) {
            free(got);
            free(buf);
            return 0;
        }
        free(s->got);
        free(s->buf);
        s->got = got;
        s->buf = buf;
        s->id = id;
        s->count = count;
        s->have = 0;
        s->last = 0;
    }
    if (!s->got[piece]) {
        s->got[piece] = 1;
        s->have++;
    }
    memcpy(s->buf + (size_t) piece * SLIDE_PIECE, b, n);
    if (piece + 1 == count) s->last = n;
    if (s->have < s->count) return 0;
    s->done = id;
    s->id = -1; /* complete: the same number later is that slide again */
    return 1;
}

int rds_uecp_slide(rds_slide *s, const uint8_t *d, size_t len, const uint8_t **data, size_t *size) {
    int found = 0;
    for (size_t i = 0; i < len; i++) {
        if (d[i] != 0xFE) continue;
        size_t j = i + 1;
        while (j < len && d[j] != 0xFF) j++;
        if (j >= len) break;
        uint8_t u[UECP_MAX];
        size_t mfl = 0;
        const uint8_t *msg = message(d + i + 1, j - i - 1, u, &mfl);
        /* MEC 0x2D, length, "dl", 0x02, slide, piece (2), pieces (2), the bytes */
        if (msg && mfl >= 11 && msg[0] == 0x2D && msg[1] >= 9 && (size_t) msg[1] + 2 <= mfl && msg[2] == 'd' &&
            msg[3] == 'l' && msg[4] == 0x02 &&
            slide_piece(s, msg[5], (unsigned) (msg[6] << 8 | msg[7]), (unsigned) (msg[8] << 8 | msg[9]), msg + 10,
                        (size_t) msg[1] - 8)) {
            *data = s->buf;
            *size = (size_t) (s->count - 1) * SLIDE_PIECE + s->last;
            found = 1;
        }
        i = j;
    }
    return found;
}
