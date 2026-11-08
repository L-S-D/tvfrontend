/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "mpa_pcm.h"

#include <stdlib.h>
#include <string.h>

#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"

#define PEND_MAX 16384

struct mpa_pcm {
    mp3dec_t dec;
    uint8_t pend[PEND_MAX];
    size_t pend_len;
    int64_t next_pts90; /* PTS of the next sample to output, -1 = unknown */
    int16_t pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
    int16_t stereo[MINIMP3_MAX_SAMPLES_PER_FRAME * 2];
};

mpa_pcm *mpa_pcm_new(void) {
    mpa_pcm *m = calloc(1, sizeof(*m));
    if (!m) return NULL;
    mp3dec_init(&m->dec);
    m->next_pts90 = -1;
    return m;
}

void mpa_pcm_free(mpa_pcm *m) { free(m); }

void mpa_pcm_push(mpa_pcm *m, const uint8_t *data, size_t len, int64_t pts90, mpa_pcm_out out, void *user) {
    /* DVB PES are frame-aligned: when nothing is pending, the PES PTS applies */
    if (pts90 >= 0 && (m->pend_len == 0 || m->next_pts90 < 0)) m->next_pts90 = pts90;
    if (len > PEND_MAX - m->pend_len) {
        m->pend_len = 0; /* overflow (corrupt stream): start over */
        if (len > PEND_MAX) return;
        if (pts90 >= 0) m->next_pts90 = pts90;
    }
    memcpy(m->pend + m->pend_len, data, len);
    m->pend_len += len;

    size_t pos = 0;
    while (pos < m->pend_len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(&m->dec, m->pend + pos, (int) (m->pend_len - pos), m->pcm, &info);
        if (info.frame_bytes <= 0) break; /* needs more data */
        pos += (size_t) info.frame_bytes;
        if (samples <= 0 || info.hz <= 0) continue; /* skipped garbage */
        const int16_t *pcm = m->pcm;
        if (info.channels == 1) {
            for (int i = 0; i < samples; i++) m->stereo[2 * i] = m->stereo[2 * i + 1] = m->pcm[i];
            pcm = m->stereo;
        }
        if (m->next_pts90 >= 0) {
            out(user, pcm, samples, m->next_pts90, info.hz);
            m->next_pts90 = (m->next_pts90 + (int64_t) samples * 90000 / info.hz) & ((1LL << 33) - 1);
        }
    }
    if (pos > 0) {
        memmove(m->pend, m->pend + pos, m->pend_len - pos);
        m->pend_len -= pos;
    }
}
