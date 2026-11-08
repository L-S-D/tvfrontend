#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds (if needed) and packages the IPK: com.lsd.tvf_<version>_arm.ipk
set -e
SDK="${WEBOS_SDK_PATH:-/opt/webos-sdk/arm-webos-linux-gnueabi_sdk-buildroot}"
DIR="$(cd "$(dirname "$0")" && pwd)"
"$DIR/build.sh"
PKG="$DIR/pkg"
rm -rf "$PKG"
mkdir -p "$PKG/res/fonts" "$PKG/res/hbbtv" "$PKG/res/lang" "$PKG/res/langnames"
cp "$DIR/appinfo.json" "$DIR/icon.png" "$DIR/largeIcon.png" "$PKG/"
"$SDK/bin/arm-webos-linux-gnueabi-strip" -o "$PKG/tvf" "$DIR/build/tvf"
cp "$DIR"/res/fonts/*.ttf "$DIR"/res/fonts/*.otf "$DIR"/res/fonts/LICENSE-* "$DIR"/res/fonts/README-* "$PKG/res/fonts/"
cp "$DIR"/res/hbbtv/*.js "$DIR"/res/hbbtv/LICENSE-* "$PKG/res/hbbtv/"
for f in "$DIR"/res/hbbtv/*.json; do
    case "$f" in */egress.*.json) ;; *) cp "$f" "$PKG/res/hbbtv/" ;; esac
done
# local egress rules (not in the repo) instead of the empty default
if [ -f "$DIR/res/hbbtv/egress.local.json" ]; then
    cp "$DIR/res/hbbtv/egress.local.json" "$PKG/res/hbbtv/egress.json"
    echo "HbbTV egress rules: res/hbbtv/egress.local.json"
fi
cp "$DIR"/res/ca-certificates.crt "$PKG/res/"
# license notices (THIRD_PARTY_NOTICES.md)
L="$PKG/licenses"
mkdir -p "$L"
cp "$DIR"/THIRD_PARTY_NOTICES.md "$DIR"/licenses/*.txt "$L/"
[ -f "$DIR/LICENSE" ] && cp "$DIR/LICENSE" "$L/TVF-LICENSE.txt"
LV="$DIR/third_party/lvgl"
cp "$LV/LICENCE.txt" "$L/LVGL.txt"
cp "$LV/src/libs/lodepng/LICENSE.txt" "$L/lodepng.txt"
cp "$LV/src/libs/tjpgd/LICENSE.txt" "$L/tjpgd.txt"
cp "$LV/src/libs/tiny_ttf/LICENSE.txt" "$L/tiny_ttf.txt"
cp "$LV/scripts/built_in_font/font_license/Montserrat/OFL.txt" "$L/Montserrat-OFL.txt"
cp "$LV/scripts/built_in_font/font_license/FontAwesome5/LICENSE.txt" "$L/FontAwesome5.txt"
cp "$DIR/third_party/cJSON/LICENSE" "$L/cJSON.txt"
cp "$DIR/third_party/minimp3/LICENSE" "$L/minimp3.txt"
cp "$DIR/third_party/cdm/LICENSE" "$L/cdm.txt"
cp "$DIR"/res/lang/*.json "$PKG/res/lang/"
cp "$DIR"/res/langnames/*.json "$PKG/res/langnames/"
cd "$DIR"
# ares-package wants to see a terminal
script -qec "ares-package '$PKG' -o '$DIR'" /dev/null
rm -rf "$PKG"
ls -la "$DIR"/*.ipk
