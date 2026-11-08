/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Test bench for src/app/webmedia.c on the host: TVF's real media server
 * with an emulated decoder (fake_smp.c) on the abstract socket
 * @tvf-media-test, main loop as in TVF (webmedia_poll after every wakeup,
 * otherwise every 20 ms). Live video and video window are only logged.
 * Counterpart: WebKit under qemu with TVF_MEDIA_SOCK=@tvf-media-test
 * (wpe-webos sdk/tests/mse, see build.sh).
 */
#include "log.h"
#include "webmedia.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int woken;

static void wake(void) {
    pthread_mutex_lock(&lk);
    woken = 1;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&lk);
}

/* TVF_TEST_LIVE_MS: releasing the live video takes this long (like unloading
 * the live player on the TV, ~60-300 ms) - the feeder's lock is free meanwhile */
static void live(int on) {
    mt_log("[test] live video %s", on ? "ON" : "OFF");
    const char *ms = getenv("TVF_TEST_LIVE_MS");
    if (!on && ms) usleep((useconds_t) atoi(ms) * 1000);
}
static void video(int x, int y, int w, int h) { mt_log("[test] video window %d,%d %dx%d", x, y, w, h); }
static const char *window_id(void) { return "test-window"; }
static void source(int w, int h) { mt_log("[test] video size %dx%d", w, h); }

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 120;
    setvbuf(stdout, NULL, _IONBF, 0); /* log immediately (also on abort) */
    static const webmedia_actions a = {live, video, window_id, source};
    /* webmedia_init uses @tvf-media-<pid>; fixed name for the test bench */
    setenv("TVF_WEBMEDIA_NAME", "@tvf-media-test", 1);
    if (webmedia_init(&a, wake, 1280) != 0) return 1;
    mt_log("[test] socket %s, running %d s", webmedia_socket(), secs);
    int64_t end = mt_now_ns() + (int64_t) secs * 1000000000LL;
    while (mt_now_ns() < end) {
        pthread_mutex_lock(&lk);
        if (!woken) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 20000000L;
            ts.tv_sec += ts.tv_nsec / 1000000000L;
            ts.tv_nsec %= 1000000000L;
            pthread_cond_timedwait(&cv, &lk, &ts);
        }
        woken = 0;
        pthread_mutex_unlock(&lk);
        webmedia_poll();
    }
    return 0;
}
