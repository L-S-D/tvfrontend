/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * CDM host: the TV's DRM modules (Chromium library CDMs) for EME in
 * WebKit.
 *
 * The TV ships its CDMs as Chromium "library CDMs" (interface
 * third_party/cdm, webOS variant): Widevine in libwidevine-wrapper.so
 * (talks to com.webos.service.widevine via Luna), PlayReady in
 * libplayreadycdm.so. Licence flow as with EME (create session ->
 * challenge -> licence server response -> keys). Decrypt()
 * does not decrypt into normal memory: it returns the sample for
 * the secure video path (Widevine: data + appended SVP metadata), which
 * goes unchanged into the BUFFERSTREAM pipeline with option.drm - this is
 * also how LG's browser does it (and Kodi with inputstream.adaptive).
 *
 * Instances are reference-counted: cdm_open hands out one reference (owner),
 * cdm_find_session another one (feeder); cdm_close closes the sessions
 * and drops the owner reference, the module is only destroyed with the
 * last one. Events come from the module's threads.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct tvf_cdm tvf_cdm;

/* Key status as in OpenCDM (KeyStatus) */
enum {
    CDM_KEY_USABLE = 0,
    CDM_KEY_EXPIRED,
    CDM_KEY_RELEASED,
    CDM_KEY_OUTPUT_RESTRICTED,
    CDM_KEY_OUTPUT_RESTRICTED_HDCP22,
    CDM_KEY_OUTPUT_DOWNSCALED,
    CDM_KEY_PENDING,
    CDM_KEY_INTERNAL_ERROR,
};

typedef struct {
    const uint8_t *id;
    uint32_t len;
    int status; /* CDM_KEY_* */
} cdm_key;

typedef struct {
    /* type: 0 licence request, 1 renewal, 2 release, 3 individualization */
    void (*message)(void *user, const char *sid, int type, const uint8_t *msg, uint32_t len);
    void (*keys)(void *user, const char *sid, const cdm_key *keys, int count);
    void (*closed)(void *user, const char *sid);
} cdm_events;

/* Library for the key system present */
int cdm_supported(const char *key_system);
/* The pipeline's option.drm.type ("WIDEVINE_MODULAR", "PLAYREADY"), NULL = unknown */
const char *cdm_drm_type(const char *key_system);
/* Key system for the system ID (UUID as in PSSH/caps), NULL = unknown */
const char *cdm_key_system_for_uuid(const char *uuid);

tvf_cdm *cdm_open(const char *key_system, const cdm_events *ev, void *user, char *err, size_t errn);
void cdm_close(tvf_cdm *c);
const char *cdm_key_system(const tvf_cdm *c);

/* blocks until the module replies; 0 = ok, otherwise error text in err */
int cdm_set_certificate(tvf_cdm *c, const uint8_t *d, uint32_t n, char *err, size_t errn);
/* init_type "cenc" | "keyids" | "webm"; persistent: session with a stored licence */
int cdm_create_session(tvf_cdm *c, int persistent, const char *init_type, const uint8_t *d, uint32_t n, char *sid,
                       size_t sidn, char *err, size_t errn);
int cdm_load_session(tvf_cdm *c, const char *sid, char *err, size_t errn);
int cdm_update_session(tvf_cdm *c, const char *sid, const uint8_t *d, uint32_t n, char *err, size_t errn);
int cdm_close_session(tvf_cdm *c, const char *sid, char *err, size_t errn);
int cdm_remove_session(tvf_cdm *c, const char *sid, char *err, size_t errn);

typedef struct {
    int scheme; /* 0 clear, 1 cenc, 2 cbcs */
    const uint8_t *kid;
    uint32_t kid_len;
    const uint8_t *iv; /* NULL: sample unencrypted (SVP data nonetheless) */
    uint32_t iv_len;
    const uint32_t *subs; /* pairs clear, encrypted */
    uint32_t nsubs;
    uint32_t crypt, skip; /* pattern (cbcs) */
    int64_t pts_us;
} cdm_sample;

/* Sample -> data for the pipeline (*out with free()); 0 = ok, otherwise
 * cdm::Status (2 = no key, 4 = error) */
int cdm_decrypt(tvf_cdm *c, int video, const cdm_sample *s, const uint8_t *data, uint32_t n, uint8_t **out,
                uint32_t *outn);

/* Instance with this session, or any open one of the key system
 * (own reference, drop it with cdm_unref); NULL = none */
tvf_cdm *cdm_find_session(const char *sid);
tvf_cdm *cdm_find_system(const char *key_system);
void cdm_unref(tvf_cdm *c);

#ifdef __cplusplus
}
#endif
