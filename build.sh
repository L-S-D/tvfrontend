#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds build/tvf with the webOS SDK (buildroot-nc4).
set -e
SDK="${WEBOS_SDK_PATH:-/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}"
DIR="$(cd "$(dirname "$0")" && pwd)"
[ -f "$DIR/third_party/lvgl/lvgl.h" ] || git -C "$DIR" submodule update --init --depth 1
# local defaults, not in the repo (e.g. TVF_DEFAULT_TVH_HOST=192.0.2.10)
# shellcheck disable=SC1091
[ -f "$DIR/local.env" ] && source "$DIR/local.env"
# shellcheck disable=SC1091
source "$SDK/environment-setup" >/dev/null 2>&1
# system cmake: the SDK's own one needs libssl.so.1.1
/usr/bin/cmake -B "$DIR/build" -S "$DIR" \
    -DCMAKE_TOOLCHAIN_FILE="$SDK/share/buildroot/toolchainfile.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DTVF_DEFAULT_TVH_HOST="${TVF_DEFAULT_TVH_HOST:-}" >/dev/null
/usr/bin/cmake --build "$DIR/build" -j"$(nproc)"
echo "built: $DIR/build/tvf"
