/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * The only C++ file: StarfishMediaAPIs is a C++ class without a stable
 * ABI. Everything that depends on the generation stays here.
 *
 * Important: Feed() returns a std::string by value that is constructed in the
 * TV's libstdc++ and destroyed here - hence the __cxx11 ABI
 * (webOS 4+). The SDK's libplayerAPIs is only a link stub; whether a symbol
 * really exists only shows on the device. Optional methods are
 * therefore resolved via dlsym on the mangled name - weak references
 * do not work for this: in the program (no PIE) their address is the
 * PLT entry, never NULL, and the call would go nowhere (webOS 4, for example,
 * lacks getVideoRenderQueueLength).
 */
#include "smp_api.h"

#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <new>
#include <string>

#include <StarfishMediaAPIs.h>
#include <appswitching-control-block/AcbAPI.h>

extern "C" {
#include "log.h"
}

namespace {
/* optional methods, NULL = does not exist on this firmware */
struct opt_fns {
    bool (*notify_foreground)(StarfishMediaAPIs *);
    bool (*set_time_to_decode)(StarfishMediaAPIs *, const char *);
    bool (*render_queue_length)(StarfishMediaAPIs *, int &);
    int64_t (*current_playtime)(StarfishMediaAPIs *);
};

const opt_fns &opt() {
    static const opt_fns f = [] {
        opt_fns r{};
#define OPT_SYM(f, n) r.f = reinterpret_cast<decltype(r.f)>(dlsym(RTLD_DEFAULT, n))
        OPT_SYM(notify_foreground, "_ZN17StarfishMediaAPIs16notifyForegroundEv");
        OPT_SYM(set_time_to_decode, "_ZN17StarfishMediaAPIs15setTimeToDecodeEPKc");
        OPT_SYM(render_queue_length, "_ZN17StarfishMediaAPIs25getVideoRenderQueueLengthERi");
        OPT_SYM(current_playtime, "_ZN17StarfishMediaAPIs18getCurrentPlaytimeEv");
#undef OPT_SYM
        return r;
    }();
    return f;
}
} // namespace

/*
 * ACB (webOS 4, e.g. OLED C8 from 2018): webOS 5+ puts the video into an
 * exported SDL window (windowId in Load). webOS 4 does not know this - there
 * the app switching control block binds the pipeline (mediaId) to the
 * video plane and wants to be told about its state changes. libAcbAPI no longer exists on
 * webOS 25, hence dlopen only when needed.
 */
namespace {
struct acb_fns {
    decltype(&AcbAPI_create) create;
    decltype(&AcbAPI_initialize) initialize;
    decltype(&AcbAPI_setSinkType) set_sink_type;
    decltype(&AcbAPI_setMediaId) set_media_id;
    decltype(&AcbAPI_setDisplayWindow) set_display_window;
    decltype(&AcbAPI_setCustomDisplayWindow) set_custom_display_window;
    /* The SDK header only knows (acbId, payload); on the device (webOS 4.4),
     * however, the function writes a task number like setState - without a
     * third parameter to a random address (crash in createTask). A
     * surplus argument does no harm on ARM. */
    int (*set_media_video_data)(long, const char *, long *);
    decltype(&AcbAPI_setState) set_state;
    decltype(&AcbAPI_finalize) finalize;
    decltype(&AcbAPI_destroy) destroy;
};
acb_fns acb;
bool acb_on;
char acb_app_id[128];
smp_acb_opts acb_opts = {PLAYER_TYPE_MSE, 0, 1};

/* live picture window (HbbTV shrinks the TV picture), w = 0: full screen */
struct {
    int x, y, w, h;
} acb_rect;

/* set the video plane; w/h = video size (0 = not yet known) */
void acb_window(long id, int w, int h) {
    long task = 0;
    int r = -1;
    bool full = acb_rect.w <= 0;
    long ox = full ? 0 : acb_rect.x, oy = full ? 0 : acb_rect.y;
    long ow = full ? 1920 : acb_rect.w, oh = full ? 1080 : acb_rect.h;
    if (acb_opts.window == 0) r = acb.set_display_window(id, ox, oy, ow, oh, full, &task);
    else if (acb_opts.window == 1 && w > 0 && h > 0)
        r = acb.set_custom_display_window(id, 0, 0, w, h, ox, oy, ow, oh, full, &task);
    if (r != -1)
        mt_log("[acb] %ld: window (mode %d, %dx%d -> %ld,%ld %ldx%ld) -> %d", id, acb_opts.window, w, h, ox, oy, ow,
               oh, r);
}

void acb_event(long id, long task, long ev, long app_state, long play_state, const char *reply) {
    mt_log("[acb] %ld: task %ld, event %ld, app %ld, playback %ld %s", id, task, ev, app_state, play_state,
           reply ? reply : "");
}

void acb_state(long id, AcbAPI_PlayState st) {
    long task = 0;
    if (id && !acb.set_state(id, APPSTATE_FOREGROUND, st, &task)) mt_log("[acb] %ld: state %d rejected", id, st);
}
} // namespace

struct smp_api {
    StarfishMediaAPIs *inner = nullptr;
    long acb = 0; /* 0 = without ACB */
    /* Play() already comes from LOADCOMPLETED, while Load() is still running. ACB
     * only accepts PLAYING after mediaId+LOADED, though (otherwise it releases the audio
     * only after ~7 s, and the picture waits that long): remember it until then */
    std::mutex acb_mu;
    bool acb_loaded = false;
    int acb_pending = -1; /* AcbAPI_PlayState, -1 = none */
    int vw = 0, vh = 0;   /* video size from VIDEO_INFO */
};

static void acb_request(smp_api *a, AcbAPI_PlayState st) {
    if (!a->acb) return;
    std::lock_guard<std::mutex> g(a->acb_mu);
    if (a->acb_loaded) acb_state(a->acb, st);
    else a->acb_pending = st;
}

extern "C" {

bool smp_api_acb_enable(const char *app_id, const smp_acb_opts *opts) {
    if (opts) acb_opts = *opts;
    if (acb_on) return true;
    void *h = dlopen("libAcbAPI.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        mt_log("[acb] libAcbAPI missing: %s", dlerror());
        return false;
    }
#define ACB_SYM(f, n) acb.f = reinterpret_cast<decltype(acb.f)>(dlsym(h, n))
    ACB_SYM(create, "AcbAPI_create");
    ACB_SYM(initialize, "AcbAPI_initialize");
    ACB_SYM(set_sink_type, "AcbAPI_setSinkType");
    ACB_SYM(set_media_id, "AcbAPI_setMediaId");
    ACB_SYM(set_display_window, "AcbAPI_setDisplayWindow");
    ACB_SYM(set_custom_display_window, "AcbAPI_setCustomDisplayWindow");
    ACB_SYM(set_media_video_data, "AcbAPI_setMediaVideoData");
    ACB_SYM(set_state, "AcbAPI_setState");
    ACB_SYM(finalize, "AcbAPI_finalize");
    ACB_SYM(destroy, "AcbAPI_destroy");
#undef ACB_SYM
    if (!acb.create || !acb.initialize || !acb.set_sink_type || !acb.set_media_id || !acb.set_display_window ||
        !acb.set_custom_display_window || !acb.set_media_video_data || !acb.set_state || !acb.finalize ||
        !acb.destroy) {
        mt_log("[acb] libAcbAPI incomplete (functions missing)");
        return false;
    }
    std::snprintf(acb_app_id, sizeof(acb_app_id), "%s", app_id ? app_id : "");
    acb_on = true;
    mt_log("[acb] on: player type %d, window mode %d, video data %d", acb_opts.player_type, acb_opts.window,
           acb_opts.video_data);
    return true;
}

void smp_api_set_rect(smp_api *a, int x, int y, int w, int h) {
    acb_rect.x = x;
    acb_rect.y = y;
    acb_rect.w = w;
    acb_rect.h = h;
    if (!a || !a->acb) return;
    std::lock_guard<std::mutex> g(a->acb_mu);
    if (a->acb_loaded) acb_window(a->acb, a->vw, a->vh);
}

void smp_api_video_info(smp_api *a, const char *json, int width, int height) {
    if (!a || !a->acb) return;
    std::lock_guard<std::mutex> g(a->acb_mu);
    a->vw = width;
    a->vh = height;
    if (acb_opts.video_data && json) {
        long task = 0;
        int r = acb.set_media_video_data(a->acb, json, &task);
        mt_log("[acb] %ld: video data %dx%d -> %d", a->acb, width, height, r);
    }
    acb_window(a->acb, width, height);
}

smp_api *smp_api_create(void) {
    smp_api *a = new (std::nothrow) smp_api();
    if (!a) return nullptr;
    a->inner = new (std::nothrow) StarfishMediaAPIs(nullptr);
    if (!a->inner) {
        delete a;
        return nullptr;
    }
    if (acb_on) {
        a->acb = acb.create();
        if (a->acb &&
            !acb.initialize(a->acb, static_cast<AcbAPI_PlayerType>(acb_opts.player_type), acb_app_id, acb_event)) {
            mt_log("[acb] initialize rejected");
            acb.destroy(a->acb);
            a->acb = 0;
        }
    }
    return a;
}

void smp_api_destroy(smp_api *a) {
    if (!a) return;
    if (a->acb) {
        acb.finalize(a->acb);
        acb.destroy(a->acb);
    }
    delete a->inner;
    delete a;
}

bool smp_api_notify_foreground(smp_api *a) {
    if (!opt().notify_foreground) return false;
    return opt().notify_foreground(a->inner);
}

const char *smp_api_media_id(smp_api *a) { return a->inner->getMediaID(); }

bool smp_api_load(smp_api *a, const char *payload, smp_event_fn cb, void *user) {
    if (!a->inner->Load(payload, cb, user)) return false;
    if (a->acb) {
        /* bind the pipeline to the video plane, filling the screen; report an
         * already requested Play()/Pause() only afterwards */
        std::lock_guard<std::mutex> g(a->acb_mu);
        long task = 0;
        const char *mid = a->inner->getMediaID();
        bool sink = acb.set_sink_type(a->acb, SINK_TYPE_MAIN);
        bool media = mid && acb.set_media_id(a->acb, mid);
        acb_window(a->acb, 0, 0);
        int st = acb.set_state(a->acb, APPSTATE_FOREGROUND, PLAYSTATE_LOADED, &task);
        mt_log("[acb] %ld: mediaId %s - Sink %d, Media %d, LOADED %d%s", a->acb, mid ? mid : "?", sink, media, st,
               a->acb_pending >= 0 ? ", then pending state" : "");
        a->acb_loaded = true;
        if (a->acb_pending >= 0) acb_state(a->acb, static_cast<AcbAPI_PlayState>(a->acb_pending));
        a->acb_pending = -1;
    }
    return true;
}

bool smp_api_play(smp_api *a) {
    bool ok = a->inner->Play();
    acb_request(a, PLAYSTATE_PLAYING);
    return ok;
}

bool smp_api_seek(smp_api *a, const char *millis) { return a->inner->Seek(millis); }

bool smp_api_pause(smp_api *a) {
    bool ok = a->inner->Pause();
    acb_request(a, PLAYSTATE_PAUSED);
    return ok;
}

bool smp_api_unload(smp_api *a) {
    bool ok = a->inner->Unload();
    if (a->acb) {
        std::lock_guard<std::mutex> g(a->acb_mu);
        if (a->acb_loaded) acb_state(a->acb, PLAYSTATE_UNLOADED);
        a->acb_loaded = false;
        a->acb_pending = -1;
    }
    return ok;
}
bool smp_api_push_eos(smp_api *a) { return a->inner->pushEOS(); }
bool smp_api_flush(smp_api *a) { return a->inner->flush(); }

bool smp_api_set_time_to_decode(smp_api *a, const char *payload) {
    if (!opt().set_time_to_decode) return false;
    return opt().set_time_to_decode(a->inner, payload);
}

smp_feed_result smp_api_feed(smp_api *a, const void *data, unsigned size, int64_t pts_ns, int es, char *reply,
                             unsigned reply_len) {
    char payload[192];
    std::snprintf(payload, sizeof(payload), "{\"bufferAddr\":\"%p\",\"bufferSize\":%u,\"pts\":%lld,\"esData\":%d}",
                  data, size, (long long) pts_ns, es);
    std::string r = a->inner->Feed(payload);
    if (reply && reply_len) std::snprintf(reply, reply_len, "%s", r.c_str());
    if (r.find("Ok") != std::string::npos) return SMP_FEED_OK;
    if (r.find("BufferFull") != std::string::npos) return SMP_FEED_FULL;
    if (r.find("Pending") != std::string::npos) return SMP_FEED_PENDING;
    return SMP_FEED_ERROR;
}

int smp_api_render_queue_length(smp_api *a) {
    if (!opt().render_queue_length) return -1;
    int len = -1;
    if (!opt().render_queue_length(a->inner, len)) return -1;
    return len;
}

int64_t smp_api_current_playtime(smp_api *a) {
    if (!opt().current_playtime) return -1;
    return opt().current_playtime(a->inner);
}

} // extern "C"
