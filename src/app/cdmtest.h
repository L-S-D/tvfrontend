/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Debug command CDMTEST: test the TV's CDMs without WebKit (see cdmtest.c).
 */
#pragma once

/* "<key system> <licence server> <PSSH base64>" - runs in the background */
void cdmtest_start(const char *args);
