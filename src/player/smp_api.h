/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * C facade over StarfishMediaAPIs (libplayerAPIs, C++).
 *
 * Built for webOS 4+/5+: __cxx11 std::string ABI, Load() with a
 * user pointer, constructor with uid. Methods that do not exist on every
 * firmware are bound weakly and there simply report "not supported".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Events of the Load callback (PF_EVENT_T, webOS 5+). */
enum {
    SMP_EV_FRAMEREADY = 0x00,
    SMP_EV_STR_STREAMING_INFO = 0x01,
    SMP_EV_INT_BUFFER_RANGE = 0x02,
    SMP_EV_INT_DURATION = 0x03,
    SMP_EV_STR_VIDEO_INFO = 0x04,
    SMP_EV_STR_VIDEO_TRACK_INFO = 0x05,
    SMP_EV_STR_AUDIO_INFO = 0x07,
    SMP_EV_STR_AUDIO_TRACK_INFO = 0x08,
    SMP_EV_STR_BUFF_EVENT = 0x0a,
    SMP_EV_STR_SOURCE_INFO = 0x0b,
    SMP_EV_INT_NUM_PROGRAM = 0x0d,
    SMP_EV_INT_NUM_VIDEO_TRACK = 0x0e,
    SMP_EV_INT_NUM_AUDIO_TRACK = 0x0f,
    SMP_EV_STR_RESOURCE_INFO = 0x11,
    SMP_EV_INT_ERROR = 0x12,
    SMP_EV_STR_ERROR = 0x13,
    SMP_EV_LOADCOMPLETED = 0x16,
    SMP_EV_UNLOADCOMPLETED = 0x17,
    SMP_EV_SEEKDONE = 0x19,
    SMP_EV_PLAYING = 0x1a,
    SMP_EV_PAUSED = 0x1b,
    SMP_EV_ENDOFSTREAM = 0x1c,
    SMP_EV_STR_CUSTOM = 0x1d,
    SMP_EV_INT_NEED_DATA = 0x26,
    SMP_EV_INT_ENOUGH_DATA = 0x27,
    SMP_EV_INT_SVP_VDEC_READY = 0x2b,
    SMP_EV_INT_BUFFERLOW = 0x2c,
    SMP_EV_STR_BUFFERFULL = 0x2d,
    SMP_EV_STR_BUFFERLOW = 0x2e,
    SMP_EV_DROPPED_FRAME = 0x30,
};

enum { SMP_ES_VIDEO = 1, SMP_ES_AUDIO = 2 };

typedef enum { SMP_FEED_OK, SMP_FEED_FULL, SMP_FEED_PENDING, SMP_FEED_ERROR } smp_feed_result;

typedef struct smp_api smp_api;
typedef void (*smp_event_fn)(int type, int64_t num, const char *str, void *user);

/* webOS 4 (no exported SDL window): every pipeline created afterwards
 * is bound to the video plane via ACB. false = libAcbAPI missing. */
typedef struct {
    int player_type; /* AcbAPI_PlayerType (10 = MSE) */
    int window;      /* 0 = setDisplayWindow fullscreen, 1 = setCustomDisplayWindow video -> fullscreen, 2 = none */
    int video_data;  /* 1 = report VIDEO_INFO via setMediaVideoData (like the LG browser) */
} smp_acb_opts;
bool smp_api_acb_enable(const char *app_id, const smp_acb_opts *opts);

/* VIDEO_INFO of the pipeline (ACB: report video data, set the window again) */
void smp_api_video_info(smp_api *a, const char *json, int width, int height);

/* ACB: live picture in this window (1920x1080), w = 0: full screen; also applies
 * to later pipelines. No effect without ACB (exported window: player) */
void smp_api_set_rect(smp_api *a, int x, int y, int w, int h);

/* ACB for a pipeline that someone else loaded (uMediaServer via Luna,
 * hbbmedia.c): binds media_id to the main video plane in the window of
 * smp_api_set_rect and reports its state. 0 = no ACB (webOS 5+ puts the
 * picture into the exported window instead). One such pipeline at a time.
 * player_type: AcbAPI_PlayerType, < 0 = PLAYER_TYPE_VIDEO */
long smp_acb_attach(const char *media_id, int player_type);
/* The pipeline is loaded (loadCompleted, at the latest "playing"): LOADED, then
 * the state requested so far. Luna returns the mediaId before the TV services
 * know the pipeline; LOADED before that fails (avblock "Invalid Context") and
 * leaves ACB unloaded, the picture invisible. Repeated calls do nothing. */
void smp_acb_loaded(long acb);
void smp_acb_playing(long acb, bool playing); /* PLAYING or PAUSED, held until LOADED */
/* VIDEO_INFO as StarfishMediaAPIs reports it ({"video":{...}}): video data, window */
void smp_acb_video_info(long acb, const char *json, int width, int height);
void smp_acb_detach(long acb); /* UNLOADED, then gone */

smp_api *smp_api_create(void);
void smp_api_destroy(smp_api *a);

bool smp_api_notify_foreground(smp_api *a);
const char *smp_api_media_id(smp_api *a);
bool smp_api_load(smp_api *a, const char *payload, smp_event_fn cb, void *user);
bool smp_api_play(smp_api *a);
bool smp_api_pause(smp_api *a);
bool smp_api_seek(smp_api *a, const char *millis);
bool smp_api_unload(smp_api *a);
bool smp_api_push_eos(smp_api *a);
bool smp_api_flush(smp_api *a);
bool smp_api_set_time_to_decode(smp_api *a, const char *payload);

/* Feed: bufferAddr/bufferSize/pts(ns)/esData. reply receives the response. */
smp_feed_result smp_api_feed(smp_api *a, const void *data, unsigned size, int64_t pts_ns, int es,
                             char *reply, unsigned reply_len);

/* -1 if the firmware lacks the method. */
int smp_api_render_queue_length(smp_api *a);
int64_t smp_api_current_playtime(smp_api *a);

#ifdef __cplusplus
}
#endif
