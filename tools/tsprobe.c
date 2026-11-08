/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * tsprobe - the TS part of the smp backend on the PC.
 *
 * Feeds a file (or an HTTP stream) in randomly sized chunks
 * through the same demuxer and the same entry logic as on the TV and
 * shows what the player would make of it: streams, first entry point,
 * keyframes, B-frame reordering, audio headers.
 *
 *   cc -O2 -Isrc/core -Isrc/net -Isrc/player tools/tsprobe.c src/player/ts.c src/net/http.c src/net/md5.c src/core/log.c -o tsprobe -lpthread
 *   ./tsprobe file.ts
 *   ./tsprobe http://<tvh>:9981/stream/channelnumber/1?profile=pass 5
 */
#include "http.h"
#include "log.h"
#include "ts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    ts_demux ts;
    int vpid, apid;
    es_codec vc, ac;
    long vpes, apes, keys2, keys1, rai, nopts, reorder, before_key;
    int64_t first_key_pts, last_vpts, last_apts;
    long audio_back;
    int have_ainfo;
    es_audio_info ai;
    long first_key_index;
    double first_key_ms;
} probe;

static void on_pmt(void *u, const ts_pmt *pmt) {
    probe *p = u;
    printf("PMT: program %d, PMT PID %d, PCR PID %d, version %d\n", pmt->program, pmt->pmt_pid, pmt->pcr_pid,
           pmt->version);
    int best = -1;
    for (int i = 0; i < pmt->n; i++) {
        const ts_es *e = &pmt->es[i];
        printf("  PID %5d  type 0x%02x  %-14s %-3s", e->pid, e->stream_type, es_codec_name(e->codec), e->lang);
        printf("  tag %d", e->component_tag);
        if (es_is_audio(e->codec)) printf("  audio_type %d%s", e->audio_type, e->receiver_mix ? " receiver-mix" : "");
        if (e->codec == ES_DVBSUB) printf("  subtitling_type 0x%02x composition %d", e->sub_type, e->composition);
        if (e->codec == ES_TELETEXT && e->ttx_initial) printf("  initial page %03X", e->ttx_initial);
        printf("\n");
        if (p->vpid < 0 && es_is_video(e->codec)) {
            p->vpid = e->pid;
            p->vc = e->codec;
        }
        int r = -1;
        if (e->codec == ES_AC3 || e->codec == ES_EAC3) r = 40;
        else if (e->codec == ES_AAC_ADTS) r = 30;
        else if (e->codec == ES_AAC_LATM) r = 20;
        else if (e->codec == ES_MPA) r = 10;
        if (r > best) {
            best = r;
            p->apid = e->pid;
            p->ac = e->codec;
        }
    }
    for (int i = 0; i < pmt->n_ttxsub; i++)
        printf("  teletext subtitle page %03X type %d %s\n", pmt->ttxsub[i].page, pmt->ttxsub[i].type, pmt->ttxsub[i].lang);
    ts_select(&p->ts, p->vpid, p->apid);
}

static void on_pes(void *u, int pid, const uint8_t *d, size_t n, int64_t pts, int64_t dts, int rai, int cc_err) {
    (void) dts;
    (void) cc_err;
    probe *p = u;
    if (pid == p->vpid) {
        p->vpes++;
        if (pts == TS_NO_PTS) p->nopts++;
        else {
            if (p->last_vpts != TS_NO_PTS && pts < p->last_vpts) p->reorder++;
            p->last_vpts = pts;
        }
        int k = es_keyframe(p->vc, d, n);
        if (k == 2) p->keys2++;
        if (k == 1) p->keys1++;
        if (rai) p->rai++;
        if (k >= 1 && p->first_key_pts == TS_NO_PTS && pts != TS_NO_PTS) {
            p->first_key_pts = pts;
            p->first_key_index = p->vpes;
            p->first_key_ms = mt_ms();
            printf("first entry: video PES #%ld (%s, rai=%d, %zu bytes, pts %.3f s)\n", p->vpes,
                   k == 2 ? "IDR/IRAP" : "I-frame open GOP", rai, n, pts / 90000.0);
        }
        if (p->first_key_pts == TS_NO_PTS) p->before_key++;
    } else if (pid == p->apid) {
        p->apes++;
        if (!p->have_ainfo && es_audio_info_parse(p->ac, d, n, &p->ai) == 0) {
            p->have_ainfo = 1;
            printf("Audio: %s %d Hz, %d channels, AOT %d, layer %d\n", es_codec_name(p->ac), p->ai.sample_rate,
                   p->ai.channels, p->ai.aac_object_type, p->ai.layer);
        }
        if (pts != TS_NO_PTS) {
            if (p->last_apts != TS_NO_PTS && pts < p->last_apts) p->audio_back++;
            p->last_apts = pts;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.ts|http://... [seconds]\n", argv[0]);
        return 2;
    }
    mt_ms();
    probe p;
    memset(&p, 0, sizeof(p));
    p.vpid = p.apid = -1;
    p.first_key_pts = p.last_vpts = p.last_apts = TS_NO_PTS;
    ts_init(&p.ts, on_pmt, on_pes, &p);
    srand(1234);

    uint8_t buf[65536];
    size_t total = 0;
    if (!strncmp(argv[1], "http://", 7)) {
        double secs = argc > 2 ? atof(argv[2]) : 5;
        http_conn c;
        if (http_open(argv[1], NULL, NULL, &c) != 0) {
            fprintf(stderr, "HTTP failed (%d)\n", c.status);
            return 1;
        }
        printf("HTTP %d %s after %.1f ms\n", c.status, c.content_type, mt_ms());
        while (mt_ms() < secs * 1000) {
            ssize_t r = http_read(&c, buf, (size_t) (1 + rand() % (int) sizeof(buf)));
            if (r <= 0) break;
            total += (size_t) r;
            ts_feed(&p.ts, buf, (size_t) r);
        }
        http_close(&c);
    } else {
        FILE *f = fopen(argv[1], "rb");
        if (!f) {
            perror(argv[1]);
            return 1;
        }
        for (;;) {
            size_t want = 1 + (size_t) (rand() % 4000); /* odd-sized chunks: test packet boundaries */
            size_t r = fread(buf, 1, want, f);
            if (r == 0) break;
            total += r;
            ts_feed(&p.ts, buf, r);
        }
        fclose(f);
    }
    ts_free(&p.ts);
    printf("%zu bytes, %llu packets, sync losses %llu, CC errors %llu, TEI %llu\n", total,
           (unsigned long long) p.ts.packets, (unsigned long long) p.ts.sync_losses,
           (unsigned long long) p.ts.cc_errors, (unsigned long long) p.ts.tei_packets);
    printf("Video %s: %ld PES (incomplete last one not counted), IDR/IRAP %ld, I-open-GOP %ld, RAI %ld, "
           "no PTS %ld, PTS going back (B-frames) %ld, before entry %ld\n",
           es_codec_name(p.vc), p.vpes, p.keys2, p.keys1, p.rai, p.nopts, p.reorder, p.before_key);
    printf("Audio %s: %ld PES, PTS going back %ld\n", es_codec_name(p.ac), p.apes, p.audio_back);
    return 0;
}
