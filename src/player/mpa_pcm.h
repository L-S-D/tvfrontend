/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * MPEG-1/2 audio (DVB: Layer II) -> PCM S16LE stereo, via minimp3.
 *
 * Fallback for channels whose audio comes only as MP2: whether Starfish in
 * BUFFERSTREAM mode accepts MP2 directly is not proven - 16-bit PCM
 * stereo, on the other hand, is (Jellyfin-native, Moonlight/ss4s).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct mpa_pcm mpa_pcm;

/* pcm: interleaved stereo, samples per channel; pts90 of the first sample. */
typedef void (*mpa_pcm_out)(void *user, const int16_t *pcm, int samples, int64_t pts90, int sample_rate);

mpa_pcm *mpa_pcm_new(void);
void mpa_pcm_free(mpa_pcm *m);

/* Append PES payload and output everything decodable. pts90 < 0 = no PTS. */
void mpa_pcm_push(mpa_pcm *m, const uint8_t *data, size_t len, int64_t pts90, mpa_pcm_out out, void *user);
