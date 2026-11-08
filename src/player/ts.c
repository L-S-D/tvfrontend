/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ts.h"

#include <stdlib.h>
#include <string.h>

#define PES_MAX (16 * 1024 * 1024)

static void pes_reset(ts_pes_asm *p, int pid) {
    p->pid = pid;
    p->len = 0;
    p->expected = 0;
    p->expected_known = 0;
    p->active = 0;
    p->rai = 0;
    p->cc = -1;
    p->cc_err = 0;
}

void ts_init(ts_demux *d, ts_pmt_cb on_pmt, ts_pes_cb on_pes, void *user) {
    memset(d, 0, sizeof(*d));
    d->on_pmt = on_pmt;
    d->on_pes = on_pes;
    d->user = user;
    d->pmt_pid = -1;
    d->pat_version = -1;
    d->chosen = -1;
    d->tsid = d->onid = -1;
    pes_reset(&d->pes[0], -1);
    pes_reset(&d->pes[1], -1);
}

void ts_select(ts_demux *d, int video_pid, int audio_pid) {
    if (d->pes[0].pid != video_pid) pes_reset(&d->pes[0], video_pid);
    if (d->pes[1].pid != audio_pid) pes_reset(&d->pes[1], audio_pid);
}

void ts_free(ts_demux *d) {
    free(d->pes[0].buf);
    free(d->pes[1].buf);
    d->pes[0].buf = d->pes[1].buf = NULL;
}

static int64_t read_ts(const uint8_t *b) {
    return ((int64_t) ((b[0] >> 1) & 7) << 30) | ((int64_t) b[1] << 22) | ((int64_t) (b[2] >> 1) << 15) |
           ((int64_t) b[3] << 7) | (int64_t) (b[4] >> 1);
}

/* ---------------- PSI ---------------- */

typedef void (*psi_done_fn)(ts_demux *d, int ctx, const uint8_t *sec, size_t len);

/* Returns the section length as soon as the section is complete, otherwise 0. */
static size_t psi_append(ts_psi_asm *a, const uint8_t *p, size_t n) {
    if (a->len + n > sizeof(a->buf)) n = sizeof(a->buf) - a->len;
    memcpy(a->buf + a->len, p, n);
    a->len += n;
    if (a->len < 3) return 0;
    if (a->buf[0] == 0xff) { /* stuffing bytes */
        a->active = 0;
        return 0;
    }
    size_t need = 3 + (((size_t) (a->buf[1] & 0x0f) << 8) | a->buf[2]);
    if (need > sizeof(a->buf)) {
        a->active = 0;
        return 0;
    }
    return a->len >= need ? need : 0;
}

static void psi_packet(ts_demux *d, ts_psi_asm *a, const uint8_t *pl, size_t plen, int pusi, psi_done_fn done,
                       int ctx) {
    if (pusi) {
        size_t ptr = pl[0];
        if (1 + ptr > plen) {
            a->active = 0;
            return;
        }
        /* bytes before the pointer complete the previous section */
        size_t n;
        if (a->active && ptr > 0 && (n = psi_append(a, pl + 1, ptr)) != 0) done(d, ctx, a->buf, n);
        a->active = 1;
        a->len = 0;
        if ((n = psi_append(a, pl + 1 + ptr, plen - 1 - ptr)) != 0) {
            a->active = 0;
            done(d, ctx, a->buf, n);
        }
    } else if (a->active) {
        size_t n = psi_append(a, pl, plen);
        if (n) {
            a->active = 0;
            done(d, ctx, a->buf, n);
        }
    }
}

static void choose(ts_demux *d, int i) {
    d->chosen = i;
    d->pmt_pid = d->prog[i].pid;
    if (d->on_pmt) d->on_pmt(d->user, &d->prog[i].pmt);
}

static int pmt_video_pid(const ts_pmt *pmt) {
    for (int k = 0; k < pmt->n; k++) {
        es_codec c = pmt->es[k].codec;
        if (c == ES_H264 || c == ES_HEVC || c == ES_MPEG2V) return pmt->es[k].pid;
    }
    return -1;
}

static void try_choose(ts_demux *d) {
    if (d->chosen >= 0 || d->nprog == 0) return;
    if (d->nprog == 1) {
        if (d->prog[0].have) choose(d, 0);
        return;
    }
    int best = -1;
    unsigned best_pkts = 0;
    for (int i = 0; i < d->nprog; i++) {
        if (!d->prog[i].have) continue;
        int vp = pmt_video_pid(&d->prog[i].pmt);
        if (vp < 0) continue;
        unsigned n = d->pid_pkts[vp];
        if (n > best_pkts) {
            best_pkts = n;
            best = i;
        }
    }
    if (best >= 0 && best_pkts >= 8) choose(d, best);
}

static void pat_done(ts_demux *d, int ctx, const uint8_t *s, size_t len) {
    (void) ctx;
    if (len < 12 || s[0] != 0x00) return;
    d->pat_seen++;
    d->tsid = (s[3] << 8) | s[4];
    int version = (s[5] >> 1) & 0x1f;
    if (version == d->pat_version) return;
    d->pat_version = version;
    int old_chosen_pid = d->chosen >= 0 ? d->prog[d->chosen].pid : -1;
    d->nprog = 0;
    d->chosen = -1;
    for (size_t k = 8; k + 4 <= len - 4 && d->nprog < TS_MAX_PROG; k += 4) {
        int prog = (s[k] << 8) | s[k + 1];
        int pid = ((s[k + 2] & 0x1f) << 8) | s[k + 3];
        if (prog == 0) continue; /* NIT */
        ts_prog *p = &d->prog[d->nprog++];
        memset(p, 0, sizeof(*p));
        p->pid = pid;
        p->program = prog;
        if (pid == old_chosen_pid) d->chosen = d->nprog - 1; /* PMT arrives anew */
    }
    if (d->chosen >= 0) d->prog[d->chosen].have = 0;
}

static void pmt_done(ts_demux *d, int i, const uint8_t *s, size_t len);

/* SDT actual (0x42): original_network_id */
static void sdt_done(ts_demux *d, int ctx, const uint8_t *s, size_t len) {
    (void) ctx;
    if (len < 15 || s[0] != 0x42) return;
    d->onid = (s[8] << 8) | s[9];
}

/* ---------------- Codecs ---------------- */

static es_codec codec_from(int st, const uint8_t *desc, size_t dl, char lang[4], int *audio_type) {
    es_codec c = ES_OTHER;
    int is_private = (st == 0x06);
    for (size_t k = 0; k + 2 <= dl;) {
        int tag = desc[k], len = desc[k + 1];
        const uint8_t *v = desc + k + 2;
        if (k + 2 + (size_t) len > dl) break;
        if (tag == 0x0a && len >= 3) {
            memcpy(lang, v, 3);
            lang[3] = 0;
            if (len >= 4) *audio_type = v[3];
        } else if (is_private) {
            if (tag == 0x6a) c = ES_AC3;
            else if (tag == 0x7a) c = ES_EAC3;
            else if (tag == 0x7b) c = ES_DTS;
            else if (tag == 0x7c && c == ES_OTHER) c = ES_AAC_ADTS;
            else if (tag == 0x56 && c == ES_OTHER) c = ES_TELETEXT;
            else if (tag == 0x59 && c == ES_OTHER) c = ES_DVBSUB;
            else if (tag == 0x05 && len >= 4) {
                if (!memcmp(v, "AC-3", 4)) c = ES_AC3;
                else if (!memcmp(v, "EAC3", 4)) c = ES_EAC3;
                else if (!memcmp(v, "HEVC", 4)) c = ES_HEVC;
            }
        }
        k += 2 + (size_t) len;
    }
    switch (st) {
    case 0x01:
    case 0x02: return ES_MPEG2V;
    case 0x1b: return ES_H264;
    case 0x24: return ES_HEVC;
    case 0x03:
    case 0x04: return ES_MPA;
    case 0x0f: return ES_AAC_ADTS;
    case 0x11: return ES_AAC_LATM;
    case 0x81: return ES_AC3;
    case 0x87: return ES_EAC3;
    case 0x06: return c;
    default: return ES_OTHER;
    }
}

/* Audio type from the component_type of an AC-3 or Enhanced AC-3 descriptor (EN 300 468 annex D):
 * service_type 2 = visually impaired (audio description), 3 = hearing impaired; with
 * full_service_flag 0 the stream is to be combined with a main service (receiver mix). */
static void ac3_component(const uint8_t *v, int len, int *audio_type, int *mix) {
    if (len < 2 || !(v[0] & 0x80)) return; /* component_type_flag */
    int service_type = (v[1] >> 3) & 7;
    if (service_type) *mix = !(v[1] & 0x40);
    if (service_type == 2) *audio_type = 3;
    else if (service_type == 3) *audio_type = 2;
}

/* The same from the AAC_type of an AAC descriptor (component_type values of stream_content 6) */
static void aac_type(const uint8_t *v, int len, int *audio_type, int *mix) {
    if (len < 3 || !(v[1] & 0x80)) return; /* AAC_type_flag */
    int t = v[2];
    *mix = t == 0x42 || t == 0x46 || t == 0x47 || t == 0x49;
    if (t == 0x40 || t == 0x44 || t == 0x47 || t == 0x48 || t == 0x49 || t == 0x4a) *audio_type = 3;
    else if (t == 0x41 || t == 0x45) *audio_type = 2;
}

static void ttxsub_add(ts_pmt *pmt, int page, int type, const uint8_t *lang) {
    for (int i = 0; i < pmt->n_ttxsub; i++)
        if (pmt->ttxsub[i].page == page) return;
    if (pmt->n_ttxsub >= TS_MAX_TTXSUB) return;
    ts_ttxsub *t = &pmt->ttxsub[pmt->n_ttxsub++];
    t->page = page;
    t->type = type;
    size_t o = 0;
    for (int i = 0; i < 3; i++)
        if ((lang[i] >= 'a' && lang[i] <= 'z') || (lang[i] >= 'A' && lang[i] <= 'Z')) t->lang[o++] = (char) (lang[i] | 0x20);
    t->lang[o] = 0;
}

/* What the descriptors of an elementary stream say beyond its codec: component tag, subtitling
 * type and composition page, teletext initial page and subtitle pages, audio type and receiver mix
 * (what tvh with the HTSP extensions would send, for a tvh without them). */
static void es_descriptors(ts_pmt *pmt, ts_es *e, const uint8_t *desc, size_t dl) {
    int codec_type = -1, codec_mix = -1, supp_type = -1, supp_mix = -1;
    for (size_t k = 0; k + 2 <= dl;) {
        int tag = desc[k], len = desc[k + 1];
        const uint8_t *v = desc + k + 2;
        if (k + 2 + (size_t) len > dl) break;
        if (tag == 0x52 && len >= 1) {
            e->component_tag = v[0];
        } else if (tag == 0x59 && len >= 8) { /* the first subtitling entry */
            if (!e->sub_type) {
                e->sub_type = v[3];
                e->composition = (v[4] << 8) | v[5];
            }
            if (!e->lang[0]) {
                memcpy(e->lang, v, 3);
                e->lang[3] = 0;
            }
        } else if (tag == 0x56 || tag == 0x46) { /* teletext (VBI teletext) */
            for (int o = 0; o + 5 <= len; o += 5) {
                int type = v[o + 3] >> 3, mag = v[o + 3] & 7;
                int page = ((mag ? mag : 8) << 8) | v[o + 4];
                if (type == 1 && !e->ttx_initial) e->ttx_initial = page;
                else if (type == 2 || type == 5) ttxsub_add(pmt, page, type, v + o);
            }
        } else if (tag == 0x6a || tag == 0x7a) {
            ac3_component(v, len, &codec_type, &codec_mix);
        } else if (tag == 0x7c) {
            aac_type(v, len, &codec_type, &codec_mix);
        } else if (tag == 0x7f && len >= 2 && v[0] == 0x06) { /* supplementary audio */
            switch ((v[1] >> 2) & 0x1f) { /* editorial_classification */
            case 0x01:
            case 0x03: supp_type = 3; break;
            case 0x02: supp_type = 2; break;
            default: break;
            }
            supp_mix = !(v[1] & 0x80); /* mix_type 0: mixed with the main audio by the receiver */
            if ((v[1] & 0x01) && len >= 5) {
                memcpy(e->lang, v + 2, 3);
                e->lang[3] = 0;
            }
        }
        k += 2 + (size_t) len;
    }
    /* the codec descriptors when the ISO 639 descriptor says nothing; the supplementary audio
     * descriptor wins */
    if (codec_type >= 0 && e->audio_type == 0) e->audio_type = codec_type;
    if (supp_type >= 0) e->audio_type = supp_type;
    e->receiver_mix = supp_mix >= 0 ? supp_mix : codec_mix >= 0 ? codec_mix : 0;
}

static void pmt_done(ts_demux *d, int i, const uint8_t *s, size_t len) {
    if (len < 16 || s[0] != 0x02) return;
    d->pmt_seen++;
    ts_prog *pr = &d->prog[i];
    int version = (s[5] >> 1) & 0x1f;
    if (pr->have && version == pr->pmt.version) return;

    ts_pmt pmt;
    memset(&pmt, 0, sizeof(pmt));
    pmt.program = (s[3] << 8) | s[4];
    pmt.pmt_pid = pr->pid;
    pmt.version = version;
    pmt.pcr_pid = ((s[8] & 0x1f) << 8) | s[9];
    size_t pil = ((size_t) (s[10] & 0x0f) << 8) | s[11];
    size_t k = 12 + pil, end = len - 4;
    while (k + 5 <= end && pmt.n < TS_MAX_ES) {
        int st = s[k];
        int pid = ((s[k + 1] & 0x1f) << 8) | s[k + 2];
        size_t il = ((size_t) (s[k + 3] & 0x0f) << 8) | s[k + 4];
        if (k + 5 + il > end) break;
        ts_es *e = &pmt.es[pmt.n++];
        e->pid = pid;
        e->stream_type = st;
        e->lang[0] = 0;
        e->audio_type = 0;
        e->component_tag = -1;
        e->codec = codec_from(st, s + k + 5, il, e->lang, &e->audio_type);
        es_descriptors(&pmt, e, s + k + 5, il);
        k += 5 + il;
    }
    int had = pr->have;
    pr->pmt = pmt;
    pr->have = 1;
    if (d->chosen == i) {
        if (d->on_pmt) d->on_pmt(d->user, &pr->pmt); /* new version of the chosen program */
    } else if (!had) {
        try_choose(d);
    }
}

/* ---------------- PES ---------------- */

static void pes_emit(ts_demux *d, ts_pes_asm *p) {
    const uint8_t *b = p->buf;
    size_t n = p->len;
    if (p->expected_known && p->expected && n > p->expected) n = p->expected;
    p->active = 0;
    if (n < 9 || b[0] != 0 || b[1] != 0 || b[2] != 1) return;
    int flags = b[7];
    size_t hl = b[8];
    if (9 + hl > n) return;
    int64_t pts = TS_NO_PTS, dts = TS_NO_PTS;
    if ((flags & 0x80) && hl >= 5) pts = read_ts(b + 9);
    if ((flags & 0x40) && hl >= 10) dts = read_ts(b + 14);
    if (d->on_pes) d->on_pes(d->user, p->pid, b + 9 + hl, n - 9 - hl, pts, dts, p->rai, p->cc_err);
}

static void pes_append(ts_pes_asm *p, const uint8_t *data, size_t len) {
    if (p->len + len > p->cap) {
        size_t nc = p->cap ? p->cap : 256 * 1024;
        while (nc < p->len + len) nc *= 2;
        if (nc > PES_MAX) {
            p->active = 0; /* corrupt stream without PES start: discard */
            p->len = 0;
            return;
        }
        uint8_t *nb = realloc(p->buf, nc);
        if (!nb) {
            p->active = 0;
            p->len = 0;
            return;
        }
        p->buf = nb;
        p->cap = nc;
    }
    memcpy(p->buf + p->len, data, len);
    p->len += len;
    if (!p->expected_known && p->len >= 6) {
        size_t pl = ((size_t) p->buf[4] << 8) | p->buf[5];
        p->expected = pl ? 6 + pl : 0;
        p->expected_known = 1;
    }
}

static void pes_packet(ts_demux *d, ts_pes_asm *p, const uint8_t *pl, size_t plen, int pusi, int rai, int cc,
                       int tsc) {
    if (tsc) {
        p->scrambled++;
        p->last_bad_off = d->offset;
    }
    if (pusi && (plen < 3 || pl[0] != 0 || pl[1] != 0 || pl[2] != 1)) {
        p->bad_start++;
        p->last_bad_off = d->offset;
    }
    if (p->cc >= 0) {
        if (cc == p->cc) return; /* duplicate packet */
        if (((p->cc + 1) & 0x0f) != cc) {
            p->cc_err = 1;
            d->cc_errors++;
        }
    }
    p->cc = cc;
    if (pusi) {
        if (p->active && p->len > 0) pes_emit(d, p);
        p->len = 0;
        p->expected = 0;
        p->expected_known = 0;
        p->active = 1;
        p->rai = rai;
        p->cc_err = 0;
    } else if (!p->active) {
        return;
    }
    pes_append(p, pl, plen);
    if (p->active && p->expected_known && p->expected && p->len >= p->expected) pes_emit(d, p);
}

static void handle_packet(ts_demux *d, const uint8_t *p) {
    d->packets++;
    if (p[1] & 0x80) {
        d->tei_packets++;
        return;
    }
    int pusi = (p[1] >> 6) & 1;
    int pid = ((p[1] & 0x1f) << 8) | p[2];
    int afc = (p[3] >> 4) & 3;
    int cc = p[3] & 0x0f;
    int tsc = (p[3] >> 6) & 3;
    size_t off = 4;
    int rai = 0;
    if (afc & 2) {
        size_t al = p[4];
        if (al > 183) return;
        if (al > 0) rai = (p[5] >> 6) & 1;
        off = 5 + al;
    }
    if (!(afc & 1) || off >= 188) return;
    const uint8_t *pl = p + off;
    size_t plen = 188 - off;
    if (d->pid_pkts[pid] < 0xffff) d->pid_pkts[pid]++;

    /* Several programs in the PAT: the first one whose video packet arrives now
     * wins - before PES dispatch, so that this packet already counts. */
    if (d->chosen < 0 && d->nprog > 1 && pid != 0) {
        for (int i = 0; i < d->nprog; i++) {
            if (d->prog[i].have && pmt_video_pid(&d->prog[i].pmt) == pid) {
                choose(d, i);
                break;
            }
        }
    }

    if (pid == 0) {
        psi_packet(d, &d->pat_sec, pl, plen, pusi, pat_done, 0);
        return;
    }
    if (pid == 0x11) { /* SDT/BAT */
        psi_packet(d, &d->sdt_sec, pl, plen, pusi, sdt_done, 0);
        return;
    }
    if (pid == d->pes[0].pid) {
        pes_packet(d, &d->pes[0], pl, plen, pusi, rai, cc, tsc);
        return;
    }
    if (pid == d->pes[1].pid) {
        pes_packet(d, &d->pes[1], pl, plen, pusi, rai, cc, tsc);
        return;
    }
    for (int i = 0; i < d->nprog; i++) {
        if (d->prog[i].pid == pid) {
            psi_packet(d, &d->prog[i].sec, pl, plen, pusi, pmt_done, i);
            break;
        }
    }
}

void ts_feed(ts_demux *d, const uint8_t *data, size_t len) {
    uint64_t base = d->fed; /* stream position of data[0] */
    const uint8_t *start = data;
    d->fed += len;
    while (len > 0) {
        if (d->carry_len > 0) {
            size_t take = 188 - d->carry_len;
            if (take > len) take = len;
            memcpy(d->carry + d->carry_len, data, take);
            d->carry_len += take;
            data += take;
            len -= take;
            if (d->carry_len == 188) {
                d->offset = base + (uint64_t) (data - start) - 188;
                if (d->carry[0] == 0x47) handle_packet(d, d->carry);
                else d->sync_losses++;
                d->carry_len = 0;
            }
            continue;
        }
        if (data[0] != 0x47) {
            /* resync: 0x47 followed (if visible) by another one a packet length later */
            size_t i = 1;
            while (i < len && !(data[i] == 0x47 && (i + 188 >= len || data[i + 188] == 0x47))) i++;
            d->sync_losses++;
            data += i;
            len -= i;
            continue;
        }
        if (len >= 188) {
            d->offset = base + (uint64_t) (data - start);
            handle_packet(d, data);
            data += 188;
            len -= 188;
        } else {
            memcpy(d->carry, data, len);
            d->carry_len = len;
            len = 0;
        }
    }
}

/* ---------------- ES helpers ---------------- */

const char *es_codec_name(es_codec c) {
    switch (c) {
    case ES_H264: return "H.264";
    case ES_HEVC: return "HEVC";
    case ES_MPEG2V: return "MPEG-2 Video";
    case ES_MPA: return "MPEG Audio";
    case ES_AAC_ADTS: return "AAC (ADTS)";
    case ES_AAC_LATM: return "AAC (LATM)";
    case ES_AC3: return "AC-3";
    case ES_EAC3: return "E-AC-3";
    case ES_DTS: return "DTS";
    case ES_TELETEXT: return "Teletext";
    case ES_DVBSUB: return "DVB subtitles";
    case ES_TEXTSUB: return "Teletext subtitles";
    case ES_OTHER: return "other";
    default: return "-";
    }
}

int es_is_video(es_codec c) { return c == ES_H264 || c == ES_HEVC || c == ES_MPEG2V; }

int es_is_audio(es_codec c) {
    return c == ES_MPA || c == ES_AAC_ADTS || c == ES_AAC_LATM || c == ES_AC3 || c == ES_EAC3 || c == ES_DTS;
}

typedef struct {
    const uint8_t *p;
    size_t n, bit;
} bitrd;

static int br_bit(bitrd *b) {
    if (b->bit >= b->n * 8) return 0;
    int v = (b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1;
    b->bit++;
    return v;
}

static unsigned br_ue(bitrd *b) {
    int zeros = 0;
    while (!br_bit(b) && zeros < 31) zeros++;
    unsigned v = 0;
    for (int i = 0; i < zeros; i++) v = (v << 1) | (unsigned) br_bit(b);
    return (1u << zeros) - 1 + v;
}

/* next start code 00 00 01 from i; returns the index of the first byte after it */
static size_t next_sc(const uint8_t *p, size_t n, size_t i) {
    while (i + 3 <= n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i + 3;
        i++;
    }
    return n;
}

int es_keyframe(es_codec c, const uint8_t *p, size_t n) {
    int has_ps = 0;
    for (size_t i = next_sc(p, n, 0); i < n; i = next_sc(p, n, i)) {
        if (c == ES_H264) {
            int t = p[i] & 0x1f;
            if (t == 7 || t == 8) has_ps = 1;
            else if (t == 5) return 2;
            else if (t == 1) {
                bitrd b = {p + i + 1, n - i - 1, 0};
                br_ue(&b); /* first_mb_in_slice */
                unsigned st = br_ue(&b) % 5;
                return (st == 2 || st == 4) && has_ps ? 1 : 0;
            }
        } else if (c == ES_HEVC) {
            int t = (p[i] >> 1) & 0x3f;
            if (t >= 32 && t <= 34) has_ps = 1;
            else if (t >= 16 && t <= 21) return has_ps ? 2 : 1;
            else if (t < 16) return 0;
        } else if (c == ES_MPEG2V) {
            if (p[i] == 0xb3) has_ps = 1;
            else if (p[i] == 0x00 && i + 2 < n) {
                int pct = (p[i + 2] >> 3) & 7;
                return (pct == 1 && has_ps) ? 2 : 0;
            }
        } else {
            return 0;
        }
    }
    return 0;
}

int es_audio_info_parse(es_codec c, const uint8_t *p, size_t n, es_audio_info *o) {
    memset(o, 0, sizeof(*o));
    static const int aac_sr[16] = {96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
                                   16000, 12000, 11025, 8000,  7350,  0,     0,     0};
    if (c == ES_AAC_ADTS) {
        for (size_t i = 0; i + 7 <= n; i++) {
            if (p[i] == 0xff && (p[i + 1] & 0xf6) == 0xf0) {
                o->aac_object_type = (p[i + 2] >> 6) + 1;
                o->sample_rate = aac_sr[(p[i + 2] >> 2) & 0x0f];
                o->channels = ((p[i + 2] & 1) << 2) | (p[i + 3] >> 6);
                return o->sample_rate ? 0 : -1;
            }
        }
        return -1;
    }
    if (c == ES_AC3 || c == ES_EAC3) {
        for (size_t i = 0; i + 7 <= n; i++) {
            if (p[i] != 0x0b || p[i + 1] != 0x77) continue;
            static const int chans[8] = {2, 1, 2, 3, 3, 4, 4, 5};
            static const int sr[3] = {48000, 44100, 32000};
            int bsid = p[i + 5] >> 3;
            if (bsid <= 10) { /* AC-3 */
                int fscod = p[i + 4] >> 6;
                if (fscod == 3) return -1;
                bitrd b = {p + i + 6, n - i - 6, 0};
                int acmod = (br_bit(&b) << 2) | (br_bit(&b) << 1) | br_bit(&b);
                if ((acmod & 1) && acmod != 1) { br_bit(&b); br_bit(&b); }
                if (acmod & 4) { br_bit(&b); br_bit(&b); }
                if (acmod == 2) { br_bit(&b); br_bit(&b); }
                int lfe = br_bit(&b);
                o->sample_rate = sr[fscod];
                o->channels = chans[acmod] + lfe;
                return 0;
            }
            /* E-AC-3 */
            int fscod = p[i + 4] >> 6;
            if (fscod == 3) {
                static const int sr2[3] = {24000, 22050, 16000};
                int f2 = (p[i + 4] >> 4) & 3;
                if (f2 == 3) return -1;
                o->sample_rate = sr2[f2];
            } else {
                o->sample_rate = sr[fscod];
            }
            int acmod = (p[i + 4] >> 1) & 7;
            o->channels = chans[acmod] + (p[i + 4] & 1);
            return 0;
        }
        return -1;
    }
    if (c == ES_MPA) {
        for (size_t i = 0; i + 4 <= n; i++) {
            if (p[i] != 0xff || (p[i + 1] & 0xe0) != 0xe0) continue;
            int ver = (p[i + 1] >> 3) & 3;  /* 3=MPEG1, 2=MPEG2, 0=MPEG2.5 */
            int layer = (p[i + 1] >> 1) & 3; /* 1=III 2=II 3=I */
            int sri = (p[i + 2] >> 2) & 3;
            if (ver == 1 || layer == 0 || sri == 3) continue;
            static const int sr1[3] = {44100, 48000, 32000};
            int sr = sr1[sri];
            if (ver == 2) sr /= 2;
            if (ver == 0) sr /= 4;
            o->sample_rate = sr;
            o->channels = ((p[i + 3] >> 6) == 3) ? 1 : 2;
            o->layer = 4 - layer;
            return 0;
        }
        return -1;
    }
    if (c == ES_AAC_LATM) {
        /* LATM configuration is in StreamMuxConfig; only defaults here. */
        o->sample_rate = 48000;
        o->channels = 2;
        o->aac_object_type = 2;
        return 0;
    }
    return -1;
}
