/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * EME for WebKit: DRM connections of the OpenCDM client (libocdm in tvf-web,
 * wpe-webos helper/tvf-ocdm.c) - the page's systems and sessions on the
 * TV's CDMs (cdm.h). Protocol: wpe-webos PROTOCOL-MEDIA.md, "EME".
 */
#pragma once

/* Serve one connection (in the media server's reader thread, after the HELLO
 * with "drm"); returns when it ends, and closes fd */
void webdrm_serve(int fd);
