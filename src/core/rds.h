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
 * UTF-8 ("" = none) - or, when the packet has it, the whole dynamic label of
 * DAB that libdvbdab sends in a frame of its own (a RadioText has 64
 * characters, a longer label comes there in parts). 1 = one was there, 0 = none. */
int rds_uecp_radiotext(const uint8_t *d, size_t len, char *out, size_t n);

/* The slideshow of a DAB service (an image now and then: the cover of the
 * title, the programme that is on, news ...). libdvbdab sends the image file
 * in the same stream, in pieces: manufacturer's specific commands (MEC 0x2D)
 * with the designation "dl" and the command 0x02, then the number of the
 * slide, the number of the piece and of the pieces (2 bytes each) and up to
 * 240 bytes of the file. A slide is sent again every 20 s under its number. */
typedef struct {
    int id;          /* the slide being collected, -1 = none */
    int done;        /* the slide that was complete last, -1 = none */
    unsigned count;  /* its pieces */
    unsigned have;
    size_t last;     /* bytes of its last piece */
    uint8_t *got;    /* per piece: there */
    uint8_t *buf;
} rds_slide;

void rds_slide_init(rds_slide *s);
void rds_slide_free(rds_slide *s);

/* The pieces of the UECP frames in a packet. 1 = a slide is complete that was
 * not before: the image file in *data and *size (valid until the next call
 * with this rds_slide); 0 = none. */
int rds_uecp_slide(rds_slide *s, const uint8_t *d, size_t len, const uint8_t **data, size_t *size);
