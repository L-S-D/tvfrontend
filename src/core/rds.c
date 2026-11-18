/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "rds.h"

#include <stdio.h>
#include <string.h>

/* IEC 62106, annex E (the RDS G0 code table), 0x80..0xFE as Unicode */
static const uint16_t g0_high[127] = {
    0x00E1, 0x00E0, 0x00E9, 0x00E8, 0x00ED, 0x00EC, 0x00F3, 0x00F2, 0x00FA, 0x00F9, 0x00D1, 0x00C7, 0x015E, 0x03B2,
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

/* one frame between 0xFE and 0xFF (both left out), still stuffed */
static int frame(const uint8_t *s, size_t len, char *out, size_t n) {
    /* unstuffing: FD 00 / FD 01 / FD 02 stand for FD / FE / FF */
    uint8_t u[300];
    size_t m = 0;
    for (size_t i = 0; i < len && m < sizeof(u); i++) {
        if (s[i] == 0xFD && i + 1 < len && s[i + 1] <= 2) u[m++] = (uint8_t) (0xFD + s[++i]);
        else u[m++] = s[i];
    }
    /* address (2), sequence counter, message field length, message field, CRC (2) */
    if (m < 6 || (size_t) u[3] + 6 != m) return 0;
    if (crc16(u, m - 2) != (uint16_t) (u[m - 2] << 8 | u[m - 1])) return 0;
    const uint8_t *msg = u + 4;
    size_t mfl = u[3];
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
        if (frame(d + i + 1, j - i - 1, t, sizeof(t))) {
            snprintf(out, n, "%s", t);
            found = 1;
        }
        i = j;
    }
    return found;
}
