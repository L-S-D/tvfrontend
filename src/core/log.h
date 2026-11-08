/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Log for the test bench.
 *
 * Every line carries the time since process start in milliseconds - with the goal
 * "picture as fast as possible" the timeline is the actual finding.
 * Optionally every line also goes via UDP to a machine on the LAN, so that
 * it can be followed without SSH access to the TV:
 *
 *     socat -u UDP-RECV:5555 -        (on the receiver)
 */
#pragma once

#include <stdint.h>

/* Redirect stdout/stderr to the file (unbuffered). */
void mt_log_init(const char *path);

/* Additionally send every line to host:port via UDP ("<pc>:5555"). */
void mt_log_udp(const char *hostport);

/* CLOCK_MONOTONIC in nanoseconds. */
int64_t mt_now_ns(void);

/* Milliseconds since mt_log_init (or the first call). */
double mt_ms(void);

void mt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
