/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Lean MPEG-TS demuxer for a single service (as TVHeadend delivers it
 * with profile=pass): PAT -> PMT -> PES of the selected PIDs.
 *
 * Deliberately without its own buffer/queue logic: every complete PES goes
 * to the caller immediately. Video PES are unbounded in DVB
 * (PES_packet_length 0) and count as complete as soon as the next one begins.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef enum {
    ES_NONE = 0,
    ES_H264,
    ES_HEVC,
    ES_MPEG2V,
    ES_MPA,      /* MPEG-1/2 audio, in DVB practically always Layer II */
    ES_AAC_ADTS,
    ES_AAC_LATM,
    ES_AC3,
    ES_EAC3,
    ES_DTS,
    ES_TELETEXT, /* EBU teletext (descriptor 0x56) */
    ES_DVBSUB,   /* DVB subtitles (descriptor 0x59) */
    ES_TEXTSUB,  /* teletext subtitle page (HTSP only: tvh creates one stream per page) */
    ES_OTHER,
} es_codec;

#define TS_MAX_ES 24
#define TS_NO_PTS (-1LL)

typedef struct {
    int pid;
    int stream_type;
    es_codec codec;
    char lang[4];
    int audio_type; /* ISO 639 descriptor: 0 normal, 1 clean effects, 2 hearing impaired, 3 audio description */
    /* HTSP of a tvh with the extensions, otherwise (and for a TS read from the PMT) from the
     * descriptors of the PMT: */
    int component_tag; /* stream identifier descriptor (HbbTV), -1 = unknown */
    int receiver_mix;  /* supplementary audio to mix with the main audio (on its own only the narrator's voice) */
    int sub_type;      /* DVB subtitles: subtitling_type (0x20..0x25 hearing impaired);
                        * teletext subtitles: 2 subtitles, 5 for the hearing impaired */
    int ttx_page;      /* teletext subtitles: page hex (0x777), 0 = unknown */
    int ttx_initial;   /* teletext: the channel's initial page hex (0x100), 0 = unknown */
    int channels;      /* audio: channel count, 0 = unknown */
    int composition;   /* DVB subtitles: page (composition_id) */
} ts_es;

/* a teletext subtitle page from a teletext descriptor (type 2 subtitles, 5 hearing impaired) */
#define TS_MAX_TTXSUB 8
typedef struct {
    int page;     /* hex, e.g. 0x777 */
    int type;     /* 2 or 5 */
    char lang[4]; /* ISO 639 */
} ts_ttxsub;

typedef struct {
    int program;    /* service_id */
    int tsid, onid; /* of the TS (PAT, SDT; HTSP: sourceinfo of a tvh with the extension), -1 = unknown */
    int pmt_pid;
    int pcr_pid;
    int version;
    int n;
    ts_es es[TS_MAX_ES];
    int n_ttxsub;
    ts_ttxsub ttxsub[TS_MAX_TTXSUB]; /* all teletext subtitle pages of the PMT */
} ts_pmt;

typedef void (*ts_pmt_cb)(void *user, const ts_pmt *pmt);
/* pts/dts in 90 kHz (33 bits) or TS_NO_PTS. rai: random access indicator
 * of the first TS packet. cc_err: packet loss within this PES. */
typedef void (*ts_pes_cb)(void *user, int pid, const uint8_t *data, size_t len, int64_t pts, int64_t dts,
                          int rai, int cc_err);

typedef struct {
    int pid;
    uint8_t *buf;
    size_t len, cap;
    size_t expected; /* 0 = unbounded / not yet known */
    int expected_known;
    int active;
    int rai;
    int cc;
    int cc_err;
    /* integrity (for debugging the output stream) */
    uint64_t scrambled;      /* TS packets with scrambling bits set */
    uint64_t bad_start;      /* PES start without 00 00 01 */
    uint64_t last_bad_off;   /* byte position of the last finding in the stream */
} ts_pes_asm;

/* Assemble a PSI section across packet boundaries (PMTs with many
 * descriptors, e.g. CA, often do not fit into one packet). */
typedef struct {
    uint8_t buf[1024 + 3];
    size_t len;
    int active;
} ts_psi_asm;

#define TS_MAX_PROG 48

typedef struct {
    int pid;     /* PMT PID according to the PAT */
    int program;
    int have;    /* PMT read */
    ts_pmt pmt;
    ts_psi_asm sec;
} ts_prog;

typedef struct {
    ts_pmt_cb on_pmt;
    ts_pes_cb on_pes;
    void *user;

    uint8_t carry[188];
    size_t carry_len;

    /* PAT -> programs. The TVH start burst from the mux buffer can carry the
     * complete PAT of the multiplex; the program chosen is the one whose
     * video packets actually arrive in the stream. */
    ts_psi_asm pat_sec;
    int pat_version;
    /* DVB IDs of the multiplex (PAT or SDT actual), -1 = unknown -
     * HbbTV apps choose their content based on them */
    int tsid, onid;
    ts_psi_asm sdt_sec;
    int nprog;
    ts_prog prog[TS_MAX_PROG];
    int chosen; /* index into prog, -1 = none yet */
    int pmt_pid;
    uint16_t pid_pkts[8192];

    ts_pes_asm pes[2]; /* [0] video, [1] audio */

    uint64_t packets;
    uint64_t sync_losses;
    uint64_t cc_errors;
    uint64_t tei_packets;
    uint64_t pat_seen, pmt_seen;
    uint64_t offset;   /* byte position of the current packet in the stream */
    uint64_t fed;      /* bytes ts_feed has received so far */
} ts_demux;

void ts_init(ts_demux *d, ts_pmt_cb on_pmt, ts_pes_cb on_pes, void *user);
/* Which PIDs are assembled into PES (-1 = none). */
void ts_select(ts_demux *d, int video_pid, int audio_pid);
void ts_feed(ts_demux *d, const uint8_t *data, size_t len);
void ts_free(ts_demux *d);

const char *es_codec_name(es_codec c);
int es_is_video(es_codec c);
int es_is_audio(es_codec c);

/*
 * Is this video PES an entry point? 2 = IDR/IRAP or MPEG-2 I with
 * sequence header, 1 = I-frame (open GOP) with parameter sets, 0 = no.
 */
int es_keyframe(es_codec c, const uint8_t *p, size_t n);

typedef struct {
    int sample_rate;
    int channels;
    int aac_object_type; /* AAC: 2 = LC, 5 = SBR ... */
    int layer;           /* MPA: 1..3 */
} es_audio_info;

/* Reads the first frame header. 0 on success. */
int es_audio_info_parse(es_codec c, const uint8_t *p, size_t n, es_audio_info *out);
