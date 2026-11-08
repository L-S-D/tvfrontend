/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * CDM host, see cdm.h.
 *
 * The modules are Chromium library CDMs in the webOS variant (version 10
 * or 11, both with the same table; Decrypt with stream type). Calls into the
 * module run one after another per instance (call); promises are awaited
 * without this lock: the modules partly resolve them from their own threads.
 * Timers, file I/O and replies to platform requests go through a
 * thread of its own per instance - never from within a callback into the module.
 */
#ifndef TARGET_WEBOS
#define TARGET_WEBOS 1
#endif
#include "cdm.h"

#include "content_decryption_module.h"
extern "C" {
#include "log.h"
}

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#define CDM_DIR "/media/developer/temp/com.lsd.tvf/cdm"

namespace {

typedef void (*InitFn)();
typedef void *(*CreateFn)(int, const char *, uint32_t, GetCdmHostFunc, void *);
typedef const char *(*VersionFn)();

struct Library {
    const char *path;
    void *handle;
    bool tried;
    InitFn init;
    CreateFn create;
    VersionFn version;
};

Library g_widevine = {"/usr/lib/libwidevine-wrapper.so", nullptr, false, nullptr, nullptr, nullptr};
Library g_playready = {"/usr/lib/libplayreadycdm.so", nullptr, false, nullptr, nullptr, nullptr};
std::mutex g_lib_lock;

bool is_widevine(const char *ks) { return ks && !strcmp(ks, "com.widevine.alpha"); }
bool is_playready(const char *ks) {
    return ks && (!strcmp(ks, "com.microsoft.playready") || !strcmp(ks, "com.microsoft.playready.recommendation"));
}

Library *library_for(const char *ks) {
    if (is_widevine(ks)) return &g_widevine;
    if (is_playready(ks)) return &g_playready;
    return nullptr;
}

bool load_library(Library *l, std::string &err) {
    std::lock_guard<std::mutex> g(g_lib_lock);
    if (l->handle) return true;
    if (l->tried) {
        err = "module not loadable";
        return false;
    }
    l->tried = true;
    /* never unload: the modules have their own threads */
    void *h = dlopen(l->path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        const char *e = dlerror();
        err = e ? e : "dlopen";
        mt_log("[cdm] %s: %s", l->path, err.c_str());
        return false;
    }
    l->init = (InitFn) dlsym(h, "InitializeCdmModule_10");
    l->create = (CreateFn) dlsym(h, "CreateCdmInstance");
    l->version = (VersionFn) dlsym(h, "GetCdmVersion");
    if (!l->init || !l->create) {
        err = "entry points missing";
        mt_log("[cdm] %s: %s", l->path, err.c_str());
        return false;
    }
    l->init();
    l->handle = h;
    mt_log("[cdm] %s loaded (version %s)", l->path, l->version ? l->version() : "?");
    return true;
}

void mkdirs(const std::string &path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0700);
}

std::string hex(const uint8_t *d, uint32_t n) {
    static const char *x = "0123456789abcdef";
    std::string s;
    for (uint32_t i = 0; i < n && i < 32; i++) {
        s += x[d[i] >> 4];
        s += x[d[i] & 15];
    }
    return s;
}

int key_status(cdm::KeyStatus s) {
    switch (s) {
    case cdm::kUsable: return CDM_KEY_USABLE;
    case cdm::kExpired: return CDM_KEY_EXPIRED;
    case cdm::kOutputRestricted: return CDM_KEY_OUTPUT_RESTRICTED;
    case cdm::kOutputDownscaled: return CDM_KEY_OUTPUT_DOWNSCALED;
    case cdm::kStatusPending: return CDM_KEY_PENDING;
    case cdm::kReleased: return CDM_KEY_RELEASED;
    default: return CDM_KEY_INTERNAL_ERROR;
    }
}

/* Scheduled tasks, one thread */
class Tasks {
public:
    Tasks() : th_([this] { run(); }) {}
    ~Tasks() {
        {
            std::lock_guard<std::mutex> g(m_);
            stop_ = true;
        }
        cv_.notify_all();
        th_.join();
    }
    void post(int64_t delay_ms, std::function<void()> f) {
        auto at = std::chrono::steady_clock::now() + std::chrono::milliseconds(delay_ms > 0 ? delay_ms : 0);
        std::lock_guard<std::mutex> g(m_);
        q_.emplace(std::make_pair(at, seq_++), std::move(f));
        cv_.notify_all();
    }

private:
    void run() {
        std::unique_lock<std::mutex> l(m_);
        while (!stop_) {
            if (q_.empty()) {
                cv_.wait(l);
                continue;
            }
            auto it = q_.begin();
            if (it->first.first > std::chrono::steady_clock::now()) {
                cv_.wait_until(l, it->first.first);
                continue;
            }
            std::function<void()> f = std::move(it->second);
            q_.erase(it);
            l.unlock();
            f();
            l.lock();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::pair<std::chrono::steady_clock::time_point, uint64_t>, std::function<void()>> q_;
    uint64_t seq_ = 0;
    bool stop_ = false;
    std::thread th_; /* last: starts with fully constructed members */
};

class SimpleBuffer final : public cdm::Buffer {
public:
    explicit SimpleBuffer(uint32_t cap) : data_((uint8_t *) malloc(cap ? cap : 1)), cap_(data_ ? cap : 0) {}
    void Destroy() override {
        free(data_);
        delete this;
    }
    uint32_t Capacity() const override { return cap_; }
    uint8_t *Data() override { return data_; }
    void SetSize(uint32_t s) override { size_ = s <= cap_ ? s : cap_; }
    uint32_t Size() const override { return size_; }
    /* take over the data (malloc), then Destroy() */
    uint8_t *take() {
        uint8_t *d = data_;
        data_ = nullptr;
        return d;
    }

private:
    ~SimpleBuffer() override {}
    uint8_t *data_;
    uint32_t cap_, size_ = 0;
};

class Block final : public cdm::DecryptedBlock {
public:
    void SetDecryptedBuffer(cdm::Buffer *b) override { buf = b; }
    cdm::Buffer *DecryptedBuffer() override { return buf; }
    void SetTimestamp(int64_t t) override { ts = t; }
    int64_t Timestamp() const override { return ts; }
    cdm::Buffer *buf = nullptr;
    int64_t ts = 0;
};

struct Promise {
    bool done = false, ok = false;
    std::string sid, err;
};

} // namespace

struct tvf_cdm;

namespace {

class Host final : public cdm::Host_10, public cdm::Host_11 {
public:
    explicit Host(tvf_cdm *c) : c_(c) {}
    cdm::Buffer *Allocate(uint32_t capacity) override { return new SimpleBuffer(capacity); }
    void SetTimer(int64_t delay_ms, void *context) override;
    cdm::Time GetCurrentWallTime() override {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (double) ts.tv_sec + ts.tv_nsec / 1e9;
    }
    void OnInitialized(bool success) override;
    void OnResolveKeyStatusPromise(uint32_t promise_id, cdm::KeyStatus) override;
    void OnResolveNewSessionPromise(uint32_t promise_id, const char *session_id, uint32_t session_id_size) override;
    void OnResolvePromise(uint32_t promise_id) override;
    void OnRejectPromise(uint32_t promise_id, cdm::Exception exception, uint32_t system_code,
                         const char *error_message, uint32_t error_message_size) override;
    void OnSessionMessage(const char *session_id, uint32_t session_id_size, cdm::MessageType message_type,
                          const char *message, uint32_t message_size) override;
    void OnSessionKeysChange(const char *session_id, uint32_t session_id_size, bool has_additional_usable_key,
                             const cdm::KeyInformation *keys_info, uint32_t keys_info_count) override;
    void OnExpirationChange(const char *session_id, uint32_t session_id_size, cdm::Time new_expiry_time) override;
    void OnSessionClosed(const char *session_id, uint32_t session_id_size) override;
    void SendPlatformChallenge(const char *, uint32_t, const char *, uint32_t) override;
    void EnableOutputProtection(uint32_t) override {}
    void QueryOutputProtectionStatus() override;
    void OnDeferredInitializationDone(cdm::StreamType, cdm::Status) override {}
    cdm::FileIO *CreateFileIO(cdm::FileIOClient *client) override;
    void RequestStorageId(uint32_t version) override;
    void ReportMetrics(cdm::MetricName, uint64_t) override {}

private:
    tvf_cdm *c_;
};

} // namespace

struct tvf_cdm {
    tvf_cdm(const char *k, const cdm_events *e, void *u) : ks(k), ev(*e), user(u), host(this) {}
    std::atomic<int> refs{1};
    std::string ks, dir;
    cdm_events ev;
    void *user;
    Host host;
    /* both versions have the same table */
    cdm::ContentDecryptionModule_11 *mod = nullptr;
    int version = 0;
    std::recursive_mutex call; /* calls into the module */
    std::mutex lock;           /* everything below */
    std::condition_variable cv;
    std::map<uint32_t, Promise> promises;
    std::set<std::string> sessions;
    std::vector<std::string> usable; /* usable key IDs (for clear samples without their own) */
    /* LG's Widevine wrapper resolves the create promise without a session ID - it
     * is only in the message before it */
    std::string last_new_sid;
    uint64_t new_sid_seq = 0;
    bool closed = false, init_done = false, init_ok = false;
    Tasks *tasks = nullptr;
};

namespace {

std::mutex g_reg;
std::vector<tvf_cdm *> g_open;
std::atomic<uint32_t> g_next_promise{1};

void *get_host(int version, void *user) {
    tvf_cdm *c = (tvf_cdm *) user;
    if (version == cdm::Host_10::kVersion) return static_cast<cdm::Host_10 *>(&c->host);
    if (version == cdm::Host_11::kVersion) return static_cast<cdm::Host_11 *>(&c->host);
    return nullptr;
}

void settle(tvf_cdm *c, uint32_t id, bool ok, const std::string &sid, const std::string &err) {
    std::lock_guard<std::mutex> g(c->lock);
    auto it = c->promises.find(id);
    if (it == c->promises.end()) return; /* too late (timeout) */
    /* the first resolution counts: LG's Widevine wrapper reports the creation
     * twice (with session ID, and after the message once more without) */
    if (it->second.done) return;
    it->second.done = true;
    it->second.ok = ok;
    it->second.sid = sid;
    it->second.err = err;
    if (ok && !sid.empty()) c->sessions.insert(sid);
    c->cv.notify_all();
}

/* Call with a promise, waits for the result */
int run_op(tvf_cdm *c, const char *what, const std::function<void(uint32_t)> &fn, int timeout_ms, std::string *sid,
           char *err, size_t errn) {
    uint32_t id = g_next_promise++;
    {
        std::lock_guard<std::mutex> g(c->lock);
        if (c->closed || !c->mod) {
            snprintf(err, errn, "CDM closed");
            return -1;
        }
        c->promises[id];
    }
    {
        std::lock_guard<std::recursive_mutex> g(c->call);
        fn(id);
    }
    std::unique_lock<std::mutex> l(c->lock);
    bool done = c->cv.wait_for(l, std::chrono::milliseconds(timeout_ms), [&] { return c->promises[id].done; });
    Promise p = c->promises[id];
    c->promises.erase(id);
    l.unlock();
    if (!done) {
        snprintf(err, errn, "%s: no response from CDM", what);
        mt_log("[cdm] %s: no response after %d ms", what, timeout_ms);
        return -1;
    }
    if (!p.ok) {
        snprintf(err, errn, "%s", p.err.empty() ? what : p.err.c_str());
        mt_log("[cdm] %s rejected: %s", what, p.err.c_str());
        return -1;
    }
    if (sid) *sid = p.sid;
    return 0;
}

/* The module's file I/O (licences, certificates) under CDM_DIR/<system> */
class File final : public cdm::FileIO {
public:
    File(tvf_cdm *c, cdm::FileIOClient *cl) : c_(c), cl_(cl) {}
    void Open(const char *name, uint32_t n) override {
        std::string s;
        for (uint32_t i = 0; i < n; i++) {
            char ch = name[i];
            s += (isalnum((unsigned char) ch) || ch == '.' || ch == '-' || ch == '_') ? ch : '_';
        }
        path_ = c_->dir + "/" + (s.empty() ? "_" : s);
        mt_log("[cdm] file %s", path_.c_str());
        post([this] { cl_->OnOpenComplete(cdm::FileIOClient::Status::kSuccess); });
    }
    void Read() override {
        post([this] {
            std::vector<uint8_t> d;
            FILE *f = fopen(path_.c_str(), "rb");
            if (f) {
                uint8_t b[4096];
                size_t r;
                while ((r = fread(b, 1, sizeof(b), f)) > 0) d.insert(d.end(), b, b + r);
                fclose(f);
            }
            mt_log("[cdm] file %s read: %zu bytes", path_.c_str(), d.size());
            cl_->OnReadComplete(cdm::FileIOClient::Status::kSuccess, d.data(), (uint32_t) d.size());
        });
    }
    void Write(const uint8_t *data, uint32_t n) override {
        std::vector<uint8_t> d(data, data + n);
        post([this, d] {
            bool ok;
            if (d.empty()) {
                ok = unlink(path_.c_str()) == 0 || errno == ENOENT;
            } else {
                std::string tmp = path_ + ".tmp";
                FILE *f = fopen(tmp.c_str(), "wb");
                ok = f && fwrite(d.data(), 1, d.size(), f) == d.size();
                if (f && fclose(f) != 0) ok = false;
                ok = ok && rename(tmp.c_str(), path_.c_str()) == 0;
            }
            mt_log("[cdm] file %s written: %zu bytes%s", path_.c_str(), d.size(), ok ? "" : " (error)");
            cl_->OnWriteComplete(ok ? cdm::FileIOClient::Status::kSuccess : cdm::FileIOClient::Status::kError);
        });
    }
    void Close() override {
        File *self = this;
        c_->tasks->post(0, [self] { delete self; });
    }

private:
    void post(std::function<void()> f) {
        tvf_cdm *c = c_;
        c->tasks->post(0, [c, f] {
            std::lock_guard<std::recursive_mutex> g(c->call);
            f();
        });
    }
    tvf_cdm *c_;
    cdm::FileIOClient *cl_;
    std::string path_;
};

void Host::SetTimer(int64_t delay_ms, void *context) {
    tvf_cdm *c = c_;
    c->tasks->post(delay_ms, [c, context] {
        std::lock_guard<std::recursive_mutex> g(c->call);
        if (c->mod) c->mod->TimerExpired(context);
    });
}

void Host::OnInitialized(bool success) {
    std::lock_guard<std::mutex> g(c_->lock);
    c_->init_done = true;
    c_->init_ok = success;
    c_->cv.notify_all();
}

void Host::OnResolveKeyStatusPromise(uint32_t promise_id, cdm::KeyStatus) { settle(c_, promise_id, true, "", ""); }

void Host::OnResolveNewSessionPromise(uint32_t promise_id, const char *session_id, uint32_t session_id_size) {
    std::string sid(session_id ? session_id : "", session_id ? session_id_size : 0);
    mt_log("[cdm] promise %u: session \"%s\"", (unsigned) promise_id, sid.c_str());
    /* LoadSession without a stored session: empty session ID */
    if (sid.empty()) settle(c_, promise_id, false, "", "SessionNotFound");
    else settle(c_, promise_id, true, sid, "");
}

void Host::OnResolvePromise(uint32_t promise_id) {
    mt_log("[cdm] promise %u resolved", (unsigned) promise_id);
    settle(c_, promise_id, true, "", "");
}

void Host::OnRejectPromise(uint32_t promise_id, cdm::Exception exception, uint32_t system_code,
                           const char *error_message, uint32_t error_message_size) {
    char e[256];
    snprintf(e, sizeof(e), "%.*s (exception %u, code %u)", (int) (error_message ? error_message_size : 0),
             error_message ? error_message : "", (unsigned) exception, (unsigned) system_code);
    settle(c_, promise_id, false, "", e);
}

void Host::OnSessionMessage(const char *session_id, uint32_t session_id_size, cdm::MessageType message_type,
                            const char *message, uint32_t message_size) {
    std::string sid(session_id, session_id_size);
    {
        std::lock_guard<std::mutex> g(c_->lock);
        if (!sid.empty() && !c_->sessions.count(sid)) {
            c_->last_new_sid = sid;
            c_->new_sid_seq++;
        }
    }
    mt_log("[cdm] %s: message type %d, %u bytes", sid.c_str(), (int) message_type, (unsigned) message_size);
    if (c_->closed || !c_->ev.message) return;
    c_->ev.message(c_->user, sid.c_str(), (int) message_type, (const uint8_t *) message, message_size);
}

void Host::OnSessionKeysChange(const char *session_id, uint32_t session_id_size, bool has_additional_usable_key,
                               const cdm::KeyInformation *keys_info, uint32_t keys_info_count) {
    std::string sid(session_id, session_id_size);
    std::vector<cdm_key> k(keys_info_count);
    std::string list;
    {
        std::lock_guard<std::mutex> g(c_->lock);
        for (uint32_t i = 0; i < keys_info_count; i++) {
            k[i].id = keys_info[i].key_id;
            k[i].len = keys_info[i].key_id_size;
            k[i].status = key_status(keys_info[i].status);
            list += " " + hex(keys_info[i].key_id, keys_info[i].key_id_size) + "=" + std::to_string(k[i].status);
            std::string id((const char *) keys_info[i].key_id, keys_info[i].key_id_size);
            auto it = std::find(c_->usable.begin(), c_->usable.end(), id);
            if (k[i].status == CDM_KEY_USABLE && it == c_->usable.end()) c_->usable.push_back(id);
            if (k[i].status != CDM_KEY_USABLE && it != c_->usable.end()) c_->usable.erase(it);
        }
    }
    mt_log("[cdm] %s: keys%s%s", sid.c_str(), list.c_str(), has_additional_usable_key ? " (+new)" : "");
    if (c_->closed || !c_->ev.keys) return;
    c_->ev.keys(c_->user, sid.c_str(), k.data(), (int) keys_info_count);
}

void Host::OnExpirationChange(const char *session_id, uint32_t session_id_size, cdm::Time new_expiry_time) {
    mt_log("[cdm] %.*s: expires %.0f", (int) session_id_size, session_id, new_expiry_time);
}

void Host::OnSessionClosed(const char *session_id, uint32_t session_id_size) {
    std::string sid(session_id, session_id_size);
    {
        std::lock_guard<std::mutex> g(c_->lock);
        c_->sessions.erase(sid);
    }
    mt_log("[cdm] %s: closed", sid.c_str());
    if (c_->closed || !c_->ev.closed) return;
    c_->ev.closed(c_->user, sid.c_str());
}

void Host::SendPlatformChallenge(const char *, uint32_t, const char *, uint32_t) {
    tvf_cdm *c = c_;
    c->tasks->post(0, [c] {
        std::lock_guard<std::recursive_mutex> g(c->call);
        cdm::PlatformChallengeResponse r = {};
        if (c->mod) c->mod->OnPlatformChallengeResponse(r);
    });
}

void Host::QueryOutputProtectionStatus() {
    tvf_cdm *c = c_;
    c->tasks->post(0, [c] {
        std::lock_guard<std::recursive_mutex> g(c->call);
        /* the built-in panel (like inputstream.adaptive on webOS) */
        if (c->mod)
            c->mod->OnQueryOutputProtectionStatus(cdm::kQuerySucceeded, cdm::kLinkTypeInternal, cdm::kProtectionHDCP);
    });
}

cdm::FileIO *Host::CreateFileIO(cdm::FileIOClient *client) { return new File(c_, client); }

void Host::RequestStorageId(uint32_t version) {
    /* no ID (like inputstream.adaptive): with an ID of its own the Widevine
     * module takes the app for a new, unprovisioned origin
     * (2 Nov: generateRequest -> InvalidStateError) */
    tvf_cdm *c = c_;
    mt_log("[cdm] %s: storage ID v%u requested (empty)", c->ks.c_str(), (unsigned) version);
    c->tasks->post(0, [c, version] {
        std::lock_guard<std::recursive_mutex> g(c->call);
        if (c->mod) c->mod->OnStorageId(version, nullptr, 0);
    });
}

void destroy(tvf_cdm *c) {
    delete c->tasks; /* waits for running tasks */
    c->tasks = nullptr;
    {
        std::lock_guard<std::recursive_mutex> g(c->call);
        if (c->mod) c->mod->Destroy();
        c->mod = nullptr;
    }
    mt_log("[cdm] %s: instance destroyed", c->ks.c_str());
    delete c;
}

} // namespace

extern "C" {

int cdm_supported(const char *key_system) {
    /* PlayReady does not run in an app's process: the Porting Kit needs the
     * secure partition (/dev/mmcblk0p26) and root devices (/dev/lg/crypto0),
     * in the app jail -> Drm_Initialize fails (on the TV on 2 Nov, PmLog
     * DRM_TEE_PROXY_BASE_AllocTEEContext). No Luna service as with Widevine.
     * So do not offer it for EME; PlayReady stays on the OIPF path (LG's DRM service
     * and the pipeline). TVF_CDM_PLAYREADY=1 offers it anyway (experiments). */
    if (is_playready(key_system) && !getenv("TVF_CDM_PLAYREADY")) return 0;
    Library *l = library_for(key_system);
    return l && access(l->path, R_OK) == 0;
}

const char *cdm_drm_type(const char *key_system) {
    if (is_widevine(key_system)) return "WIDEVINE_MODULAR";
    if (is_playready(key_system)) return "PLAYREADY";
    return nullptr;
}

const char *cdm_key_system_for_uuid(const char *uuid) {
    if (!uuid) return nullptr;
    if (!strcasecmp(uuid, "edef8ba9-79d6-4ace-a3c8-27dcd51d21ed")) return "com.widevine.alpha";
    if (!strcasecmp(uuid, "9a04f079-9840-4286-ab92-e65be0885f95")) return "com.microsoft.playready";
    return nullptr;
}

tvf_cdm *cdm_open(const char *key_system, const cdm_events *ev, void *user, char *err, size_t errn) {
    Library *l = library_for(key_system);
    if (!l) {
        snprintf(err, errn, "unknown key system %s", key_system ? key_system : "-");
        return nullptr;
    }
    /* PlayReady creates its licence store under $HOME/.mediadrm/playready:
     * the app's HOME is not writable -> a directory of our own */
    if (is_playready(key_system)) {
        const char *h = getenv("HOME");
        if (!h || !*h || access(h, W_OK) != 0) {
            std::string home = std::string(CDM_DIR) + "/home";
            mkdirs(home + "/.mediadrm/playready");
            mt_log("[cdm] HOME %s not writable -> %s", h ? h : "-", home.c_str());
            setenv("HOME", home.c_str(), 1);
        } else {
            mkdirs(std::string(h) + "/.mediadrm/playready");
        }
    }
    std::string e;
    if (!load_library(l, e)) {
        snprintf(err, errn, "%s", e.c_str());
        return nullptr;
    }
    tvf_cdm *c = new tvf_cdm(key_system, ev, user);
    c->dir = std::string(CDM_DIR) + "/" + key_system;
    mkdirs(c->dir);
    c->tasks = new Tasks();
    int64_t t0 = mt_now_ns();
    void *m = l->create(cdm::ContentDecryptionModule_11::kVersion, key_system, (uint32_t) strlen(key_system), get_host, c);
    c->version = 11;
    if (!m) {
        m = l->create(cdm::ContentDecryptionModule_10::kVersion, key_system, (uint32_t) strlen(key_system), get_host, c);
        c->version = 10;
    }
    if (!m) {
        snprintf(err, errn, "CreateCdmInstance(%s) rejected", key_system);
        mt_log("[cdm] %s", err);
        delete c->tasks;
        delete c;
        return nullptr;
    }
    c->mod = (cdm::ContentDecryptionModule_11 *) m;
    {
        std::lock_guard<std::recursive_mutex> g(c->call);
        /* Widevine like inputstream.adaptive on webOS: without distinctive identifier,
         * without persistent state, secure decoder. PlayReady: the challenge
         * carries the device certificate (distinctive identifier), licences live in
         * the HDS (persistent) */
        bool pr = is_playready(key_system);
        c->mod->Initialize(pr, pr, true);
    }
    {
        std::unique_lock<std::mutex> lk(c->lock);
        c->cv.wait_for(lk, std::chrono::seconds(15), [c] { return c->init_done; });
    }
    if (!c->init_done || !c->init_ok) {
        snprintf(err, errn, "Initialize %s", c->init_done ? "failed" : "no response");
        mt_log("[cdm] %s: %s", key_system, err);
        destroy(c);
        return nullptr;
    }
    mt_log("[cdm] %s: instance v%d ready after %.0f ms", key_system, c->version, (mt_now_ns() - t0) / 1e6);
    if (is_playready(key_system)) {
        std::string hds = std::string(getenv("HOME") ? getenv("HOME") : "") + "/.mediadrm/playready/playready.hds";
        struct stat st;
        if (stat(hds.c_str(), &st) == 0) mt_log("[cdm] HDS %s: %lld bytes", hds.c_str(), (long long) st.st_size);
        else mt_log("[cdm] HDS %s missing (%s)", hds.c_str(), strerror(errno));
    }
    std::lock_guard<std::mutex> g(g_reg);
    g_open.push_back(c);
    return c;
}

void cdm_close(tvf_cdm *c) {
    if (!c) return;
    {
        std::lock_guard<std::mutex> g(g_reg);
        for (size_t i = 0; i < g_open.size(); i++)
            if (g_open[i] == c) g_open.erase(g_open.begin() + (long) i--);
    }
    std::set<std::string> open;
    {
        std::lock_guard<std::mutex> g(c->lock);
        open = c->sessions;
    }
    for (const std::string &sid : open) {
        char e[128];
        cdm_close_session(c, sid.c_str(), e, sizeof(e));
    }
    {
        std::lock_guard<std::mutex> g(c->lock);
        c->closed = true;
    }
    cdm_unref(c);
}

const char *cdm_key_system(const tvf_cdm *c) { return c->ks.c_str(); }

int cdm_set_certificate(tvf_cdm *c, const uint8_t *d, uint32_t n, char *err, size_t errn) {
    return run_op(
        c, "SetServerCertificate", [&](uint32_t id) { c->mod->SetServerCertificate(id, d, n); }, 10000, nullptr, err,
        errn);
}

int cdm_create_session(tvf_cdm *c, int persistent, const char *init_type, const uint8_t *d, uint32_t n, char *sid,
                       size_t sidn, char *err, size_t errn) {
    cdm::InitDataType t;
    if (!strcmp(init_type, "cenc")) t = cdm::kCenc;
    else if (!strcmp(init_type, "keyids")) t = cdm::kKeyIds;
    else if (!strcmp(init_type, "webm")) t = cdm::kWebM;
    else {
        snprintf(err, errn, "init data %s not supported", init_type);
        return -1;
    }
    cdm::SessionType st = persistent ? cdm::kPersistentLicense : cdm::kTemporary;
    std::string s;
    int64_t t0 = mt_now_ns();
    uint64_t seq0;
    {
        std::lock_guard<std::mutex> g(c->lock);
        seq0 = c->new_sid_seq;
    }
    int r = run_op(
        c, "CreateSessionAndGenerateRequest",
        [&](uint32_t id) { c->mod->CreateSessionAndGenerateRequest(id, st, t, d, n); }, 20000, &s, err, errn);
    if (r == 0 && s.empty()) {
        /* promise without session ID: take the one of the message that came during creation */
        std::lock_guard<std::mutex> g(c->lock);
        if (c->new_sid_seq != seq0 && !c->last_new_sid.empty()) {
            s = c->last_new_sid;
            c->sessions.insert(s);
        } else {
            snprintf(err, errn, "CDM returned no session ID");
            r = -1;
        }
    }
    if (r == 0) {
        snprintf(sid, sidn, "%s", s.c_str());
        mt_log("[cdm] %s: session %s (%s, %u bytes init data) after %.0f ms", c->ks.c_str(), sid, init_type,
               (unsigned) n, (mt_now_ns() - t0) / 1e6);
    }
    return r;
}

int cdm_load_session(tvf_cdm *c, const char *sid, char *err, size_t errn) {
    std::string s;
    return run_op(
        c, "LoadSession",
        [&](uint32_t id) { c->mod->LoadSession(id, cdm::kPersistentLicense, sid, (uint32_t) strlen(sid)); }, 20000,
        &s, err, errn);
}

int cdm_update_session(tvf_cdm *c, const char *sid, const uint8_t *d, uint32_t n, char *err, size_t errn) {
    int64_t t0 = mt_now_ns();
    int r = run_op(
        c, "UpdateSession", [&](uint32_t id) { c->mod->UpdateSession(id, sid, (uint32_t) strlen(sid), d, n); }, 20000,
        nullptr, err, errn);
    if (r == 0) mt_log("[cdm] %s: license (%u bytes) accepted after %.0f ms", sid, (unsigned) n, (mt_now_ns() - t0) / 1e6);
    return r;
}

int cdm_close_session(tvf_cdm *c, const char *sid, char *err, size_t errn) {
    int r = run_op(
        c, "CloseSession", [&](uint32_t id) { c->mod->CloseSession(id, sid, (uint32_t) strlen(sid)); }, 5000, nullptr,
        err, errn);
    std::lock_guard<std::mutex> g(c->lock);
    c->sessions.erase(sid);
    return r;
}

int cdm_remove_session(tvf_cdm *c, const char *sid, char *err, size_t errn) {
    return run_op(
        c, "RemoveSession", [&](uint32_t id) { c->mod->RemoveSession(id, sid, (uint32_t) strlen(sid)); }, 10000,
        nullptr, err, errn);
}

int cdm_decrypt(tvf_cdm *c, int video, const cdm_sample *s, const uint8_t *data, uint32_t n, uint8_t **out,
                uint32_t *outn) {
    cdm::InputBuffer_2 in = {};
    in.data = data;
    in.data_size = n;
    /* clear sample without key ID (lead-in): inputstream.adaptive uses the stream's - here the first usable one */
    std::string any;
    if (!s->kid || !s->kid_len) {
        std::lock_guard<std::mutex> g(c->lock);
        if (!c->usable.empty()) any = c->usable.front();
    }
    in.encryption_scheme = s->scheme == 2 ? cdm::EncryptionScheme::kCbcs
                           : s->scheme == 1 ? cdm::EncryptionScheme::kCenc
                                            : cdm::EncryptionScheme::kUnencrypted;
    in.key_id = s->kid && s->kid_len ? s->kid : (const uint8_t *) any.data();
    in.key_id_size = s->kid && s->kid_len ? s->kid_len : (uint32_t) any.size();
    in.iv = s->iv;
    /* like inputstream.adaptive: always 16, even without IV */
    in.iv_size = s->iv ? s->iv_len : 16;
    std::vector<cdm::SubsampleEntry> subs(s->nsubs);
    for (uint32_t i = 0; i < s->nsubs; i++) {
        subs[i].clear_bytes = s->subs[2 * i];
        subs[i].cipher_bytes = s->subs[2 * i + 1];
    }
    in.subsamples = subs.empty() ? nullptr : subs.data();
    in.num_subsamples = s->nsubs;
    in.pattern.crypt_byte_block = s->crypt;
    in.pattern.skip_byte_block = s->skip;
    in.timestamp = s->pts_us;
    Block blk;
    cdm::Status st;
    {
        std::lock_guard<std::recursive_mutex> g(c->call);
        if (!c->mod) return cdm::kDecryptError;
        st = c->mod->Decrypt(in, &blk, video ? cdm::kStreamTypeVideo : cdm::kStreamTypeAudio);
    }
    if (st != cdm::kSuccess || !blk.buf) {
        if (blk.buf) blk.buf->Destroy();
        return st != cdm::kSuccess ? (int) st : (int) cdm::kDecryptError;
    }
    SimpleBuffer *b = static_cast<SimpleBuffer *>(blk.buf);
    *outn = b->Size();
    *out = b->take();
    b->Destroy();
    return *out ? 0 : (int) cdm::kDecryptError;
}

tvf_cdm *cdm_find_session(const char *sid) {
    std::lock_guard<std::mutex> g(g_reg);
    for (tvf_cdm *c : g_open) {
        std::lock_guard<std::mutex> l(c->lock);
        if (c->sessions.count(sid)) {
            c->refs++;
            return c;
        }
    }
    return nullptr;
}

tvf_cdm *cdm_find_system(const char *key_system) {
    std::lock_guard<std::mutex> g(g_reg);
    for (tvf_cdm *c : g_open)
        if (library_for(c->ks.c_str()) == library_for(key_system)) {
            c->refs++;
            return c;
        }
    return nullptr;
}

void cdm_unref(tvf_cdm *c) {
    if (c && --c->refs == 0) destroy(c);
}

} // extern "C"
