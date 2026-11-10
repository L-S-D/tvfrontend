/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Live TV player: MPEG-TS over HTTP -> own demuxer -> StarfishMediaAPIs
 * (BUFFERSTREAM, contentsType LIVE), like Kodi's MediaPipelineWebOS.
 *
 * Why not NDL DirectMedia: NDL loads the same pipeline in
 * game streaming mode (lowDelayMode, isGameMode) - every frame appears on
 * arrival, the PTS is not a presentation time. With broadcast (B-frames,
 * network jitter, start burst) the picture becomes jerky. BUFFERSTREAM/LIVE has its
 * own clock. Measured on webOS 25: first frame 0.4-0.5 s (free-to-air)
 * or 0.65-0.85 s (HD+, card response ~0.3 s).
 *
 * All calls from the main thread (which owns the SDL window).
 */
#pragma once

#include "cJSON.h"
#include "ts.h"

#include <stddef.h>
#include <stdint.h>

typedef struct player player;

typedef struct {
    int active;          /* a session is running */
    int first_frame;     /* ... and has shown its first frame */
    double first_frame_ms;
    int failed;          /* session ended without showing a frame */
    int ended;           /* stream broke off/ended (feeder done) */
    int http_status;     /* server response, 0 = none (yet) */
    int http_wait_ms;    /* running session has been waiting this many ms for the response, 0 = not */
    int picture_age_ms;  /* ms without a new frame while the stream runs, -1 = not measurable (radio, pause) */
    int video_w, video_h;
    double video_fps; /* frame rate according to the decoder, 0 = unknown */
    int interlaced;
    char vcodec[24];
    char acodec[24];
    int audio_channels;
} player_status;

/* Audio track of the current program (from the PMT) */
typedef struct {
    int pid;
    char lang[4];  /* ISO 639-2, "" = unspecified */
    int type;      /* audio_type: 0 normal, 2 hearing impaired (clear speech), 3 audio description */
    int receiver_mix; /* supplementary audio to mix: on its own only the narrator's voice (tvh HTSP) */
    char codec[8]; /* "ac3", "eac3", "mp2", "aac", "latm", "dts" (names as in tvh) */
} player_track;

#define PLAYER_MAX_TRACKS 16

/* params: JSON with tuning knobs (ctype, margin, vmax, mpa, alang, stats ...),
 * NULL = defaults. See player_create(). */
player *player_create(const char *params);

/* Exported video window (SDL_webOSCreateExportedWindow) - before the
 * first Load(), the feeder waits for it. */
void player_set_window(player *p, const char *window_id);

/* webOS 4 (e.g. OLED C8 from 2018) has no exported window: video via ACB.
 * Before the first Load(), then player_set_window(p, ""). 0 = not possible. */
int player_use_acb(player *p);

/* Live picture in this window (1920x1080), w = 0: full screen - for HbbTV apps
 * that shrink the TV picture */
void player_set_video_rect(player *p, int x, int y, int w, int h);
/* Picture size for the exported window when someone else supplies the picture
 * (WebKit via webmedia.c); 0, 0 = back to our own from VIDEO_INFO */
void player_set_video_source(player *p, int w, int h);

/* Ends the running session and starts url: TS over HTTP or
 * htsp://host:port/<channel ID>[?profile=...] (subscription directly at tvh). Returns
 * immediately; connection, demux and Load run in the background. */
void player_play(player *p, const char *url);
/* Status messages of the HTSP subscription (subscriptionStart, signalStatus,
 * descrambleInfo, subscriptionStatus, subscriptionStop ...) with the channel;
 * in the player thread, msg is owned by the player */
typedef void (*player_htsp_status_fn)(unsigned long channel, const char *method, const cJSON *msg);
void player_set_htsp_status(player *p, player_htsp_status_fn fn);

/* Preference for the audio track choice of the next starts (choice remembered
 * per channel): same language+type+codec, otherwise same language. lang NULL/"" =
 * no preference (then German, no audio description, best codec). */
void player_set_audio_pref(player *p, const char *lang, int type, const char *codec);

/* Sound offset to the picture in ms: + = sound later. Shifts the time stamps of
 * the audio fed from now on (live, timeshift, recordings); what the TV has
 * buffered already plays as before. Any thread. */
void player_set_audio_offset(player *p, int ms);

/* Audio tracks of the current program; *current = index of the one playing
 * (-1 = none/not chosen yet). Returns the count (0 before the PMT). */
int player_audio_tracks(player *p, player_track *out, int max, int *current);

/* Play a media URL (HbbTV catch-up service): the pipeline fetches and decodes
 * by itself. type = "URI" (MP4 etc.), "HLS" or "MPEG-DASH". Pause/resume with
 * player_set_paused, end/stop as usual (player_stop / next play). */
void player_play_media(player *p, const char *type, const char *url);

typedef struct {
    int active;     /* media session running */
    int playing;    /* currently playing (not paused/buffering) */
    int ended;      /* finished */
    int error;      /* pipeline error (code), 0 = none */
    int64_t pos_ms; /* position, -1 = unknown */
    int64_t dur_ms; /* duration, 0 = unknown/live */
} player_media;

void player_media_get(player *p, player_media *m);

/* HLS with TS segments through our own feed (the TV's media server
 * hangs on HLS); start = seconds (VOD). State via player_media_get. */
void player_play_hls(player *p, const char *url, double start);

/* exported video window (windowId for other pipelines), "" = none */
const char *player_window_id(player *p);
void player_media_seek(player *p, double sec);

/* DVB IDs of the current program from the stream's PAT/SDT (HbbTV:
 * currentChannel). 1 = all three known */
int player_dvb_ids(player *p, int *onid, int *tsid, int *sid);

/* Components of the running HTSP subscription for HbbTV (OIPF AVComponent) */
typedef struct {
    int index;       /* stream index from tvh (audio track: player_track.pid) */
    int kind;        /* 0 video, 1 audio, 2 subtitles (OIPF COMPONENT_TYPE_*) */
    int tag;         /* component_tag, -1 = unknown (tvh without the extension) */
    char encoding[12];
    char lang[4];
    int audio_type;  /* audio: 2 hearing impaired, 3 audio description */
    int channels;    /* audio: channel count, 0 = unknown */
    int hearing;     /* subtitles for the hearing impaired */
    int ttx_page;    /* teletext subtitles: page hex, otherwise 0 */
    int composition; /* DVB subtitles: page */
    int active;      /* video or the selected audio track */
} player_component;
#define PLAYER_MAX_COMPONENTS 24
/* count, at most max (subtitles: the caller sets active) */
int player_components(player *p, player_component *out, int max);

/* Different audio track: restarts the session with this PID (like zapping,
 * ~0.5 s) - audio and video sit in the decoder seconds ahead, a
 * switch within the running stream would only arrive after this lead. */
void player_select_audio(player *p, int pid);

/* Play a file (recording, /dvrfile) from byte offset (HTTP Range);
 * pos0 = time in the recording at that point (s). Afterwards
 * player_position() returns the time of the displayed frame. Seeking = restart. */
void player_play_file(player *p, const char *url, long long offset, double pos0);

/* like player_play, but the explicit audio track choice is kept (timeshift:
 * the same channel from a different point) */
void player_play_keep_audio(player *p, const char *url);

/* Pause (file playback, timeshift; live until resumed from the buffer) */
void player_set_paused(player *p, int paused);

/* HTSP live with timeshift from tvh (htsp:// URLs): duration requested
 * with the subscription (0 = none); applies from the next player_play */
void player_set_htsp_timeshift(player *p, int seconds);
typedef struct {
    int period;      /* granted (s) */
    int have_status; /* timeshiftStatus already received */
    int full;        /* buffer full */
    long long shift90;          /* this far behind live (90 kHz) */
    long long start90, end90;   /* buffer according to tvh (90 kHz, -1 = unknown) */
} player_tshift;
/* 1 = running HTSP subscription with granted timeshift */
int player_htsp_timeshift(player *p, player_tshift *out);
/* Commands to the subscription (0 = sent): speed (0 = pause, 100 = normal),
 * relative skip (90 kHz), back to live. After a skip the player
 * re-initializes the decoder while keeping the connection (subscriptionSkip). */
int player_htsp_speed(player *p, int speed);
int player_htsp_skip(player *p, long long delta90);
int player_htsp_live(player *p);
/* Side streams of an HTSP subscription (teletext, DVB subtitles) for our own
 * page/subtitle store - called in the feeder thread, not in the main thread */
typedef struct {
    int index;    /* stream index from tvh */
    int teletext; /* 1 teletext, 0 DVB subtitles */
    int ait;      /* 1 HbbTV AIT (tvh: type HBBTV, packets = AIT sections) */
    int textsub;  /* 1 teletext subtitle page (tvh: TEXTSUB, no packets needed) */
    char lang[4];
    int composition, ancillary; /* DVB subtitles: pages */
    /* tvh with the HTSP extensions, otherwise 0: */
    int sub_type;    /* DVB subtitles subtitling_type, teletext subtitles 2/5 */
    int ttx_page;    /* teletext subtitles: page hex */
    int ttx_initial; /* teletext: the channel's initial page hex */
} player_side_stream;
typedef struct {
    /* subscription started (also after reconnecting): channel ID and its side streams */
    void (*start)(void *user, unsigned long channel, const player_side_stream *s, int n);
    /* payload of a packet as delivered by tvh (teletext from data_identifier, DVB
     * from the first segment), pts 90 kHz like the video (player_stream_pts) */
    void (*data)(void *user, int index, const uint8_t *d, size_t len, long long pts);
    /* skip in timeshift: from here on packets of the new position */
    void (*skip)(void *user);
    void *user;
} player_side;
/* before the first player_play */
void player_set_side(player *p, const player_side *s);
/* The stream list of an HTSP subscription shows none of the details a tvh with the HTSP extensions
 * sends (component tags, subtitling types, teletext pages, receiver mix): cb is called with the
 * channel (feeder thread). The app can read them from the PMT and give them to player_merge_pmt()
 * (any thread); it returns 1 when something was added to the tracks of the channel's session. */
void player_set_pmt_request(player *p, void (*cb)(void *user, unsigned long channel), void *user);
int player_merge_pmt(player *p, unsigned long channel, const ts_pmt *pmt);
/* Test (debug VDROP): discard video for this long, data keeps flowing - picture watchdog */
void player_test_drop_video(player *p, int seconds);
/* Test (debug CUT): drop the file playback connection, make the next
 * fail new requests fail (restart, 5 = give up) */
void player_test_cut(player *p, int fail);
int player_paused(player *p);

/* Position of file playback in s (before the first frame: pos0), -1 for live */
double player_position(player *p);

/* Stream PTS (90 kHz, 33 bits) of the frame currently shown, extrapolated with
 * the clock between frame reports (~5/s); -1 = no frame yet.
 * For subtitles whose events carry stream PTS. */
long long player_stream_pts(player *p);

/* File playback finished: 1 = EOS played out, or file finished and no frame
 * for 3 s; 2 = aborted before the end of the file (network, even after 5 new
 * requests from the same position), not "watched"; 0 = running */
int player_ended(player *p);

/* Switch audio track in file playback: restart from offset/pos0 */
void player_select_audio_file(player *p, int pid, long long offset, double pos0);

/* Total size of the file (Content-Range), -1 = unknown */
long long player_file_size(player *p);

/* Stop playback and release the decoder. */
void player_stop(player *p);

/* Periodically from the main thread: window geometry, statistics. */
void player_tick(player *p);

void player_status_get(player *p, player_status *st);

void player_destroy(player *p);
