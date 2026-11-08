/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int64_t t0;
static int udp_fd = -1;
static struct sockaddr_in udp_addr;
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;

int64_t mt_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t) ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

double mt_ms(void) {
    if (t0 == 0) {
        t0 = mt_now_ns();
    }
    return (double) (mt_now_ns() - t0) / 1e6;
}

void mt_log_init(const char *path) {
    if (t0 == 0) {
        t0 = mt_now_ns();
    }
    FILE *lf = fopen(path, "w");
    if (lf) {
        setvbuf(lf, NULL, _IONBF, 0);
        dup2(fileno(lf), STDOUT_FILENO);
        dup2(fileno(lf), STDERR_FILENO);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
    }
}

void mt_log_udp(const char *hostport) {
    char host[64];
    const char *colon = strrchr(hostport, ':');
    if (!colon || (size_t) (colon - hostport) >= sizeof(host)) {
        return;
    }
    memcpy(host, hostport, (size_t) (colon - hostport));
    host[colon - hostport] = 0;
    memset(&udp_addr, 0, sizeof(udp_addr));
    udp_addr.sin_family = AF_INET;
    udp_addr.sin_port = htons((uint16_t) atoi(colon + 1));
    if (inet_pton(AF_INET, host, &udp_addr.sin_addr) != 1) {
        return;
    }
    udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
}

void mt_log(const char *fmt, ...) {
    char line[2048];
    int n = snprintf(line, sizeof(line), "[%8.1f] ", mt_ms());
    va_list ap;
    va_start(ap, fmt);
    int m = vsnprintf(line + n, sizeof(line) - (size_t) n - 1, fmt, ap);
    va_end(ap);
    if (m < 0) {
        m = 0;
    }
    size_t len = (size_t) n + (size_t) m;
    if (len > sizeof(line) - 2) {
        len = sizeof(line) - 2;
    }
    line[len++] = '\n';
    line[len] = 0;

    pthread_mutex_lock(&log_lock);
    if (fwrite(line, 1, len, stdout) != len) {
        /* stdout gone (no terminal): the UDP log remains */
    }
    if (udp_fd >= 0) {
        sendto(udp_fd, line, len, MSG_DONTWAIT, (struct sockaddr *) &udp_addr, sizeof(udp_addr));
    }
    pthread_mutex_unlock(&log_lock);
}
