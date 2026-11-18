/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * RDS in UECP frames (IEC 62106-10), as DVB radio carries it next to the audio
 * and tvheadend passes it on (HTSP stream type "RDS"; libdvbdab puts the
 * dynamic label of DAB there): the RadioText.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* The RadioText of the UECP frames in a packet (one or more, each from 0xFE to
 * 0xFF): the last RadioText message (MEC 0x0A) with a good CRC into out as
 * UTF-8 ("" = none). 1 = one was there, 0 = none. */
int rds_uecp_radiotext(const uint8_t *d, size_t len, char *out, size_t n);
