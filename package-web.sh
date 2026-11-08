#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Packages a WebKit bundle (tvf-web, from wpe-webos) as a separate IPK
# com.lsd.tvf.web_<version>_all.ipk - installed next to TVF, TVF finds it on its
# own (launch parameter webBundle "auto" or no remembered path). The bundle sits
# under bundle/ in the package; version 1.<YYMMDD>.<hhmm> from the timestamp of
# the bundle (libWPEWebKit) if none is given.
#   ./package-web.sh BUNDLE_DIR [VERSION]
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
B="${1:?bundle directory missing (e.g. .../wpe-webos/sdk/out/tvf-web-sdk-media-armhf)}"
B="$(cd "$B" && pwd)"
[ -f "$B/run.sh" ] && [ -x "$B/bin/tvf-web" ] || { echo "not a tvf-web bundle: $B" >&2; exit 1; }
VER="${2:-$(date -r "$(ls "$B"/lib/libWPEWebKit-2.0.so* | head -1)" +1.%y%m%d.%H%M | sed -E 's/\.0+([0-9])/.\1/g')}"
PKG="$DIR/pkg-web"
rm -rf "$PKG"
mkdir -p "$PKG"
cp -a "$B" "$PKG/bundle"
# leave out backups (*.vor-*) and test leftovers
find "$PKG/bundle" \( -name '*.vor-*' -o -name '*.alt' \) -delete
cp "$DIR/icon.png" "$PKG/icon.png"
cat > "$PKG/appinfo.json" <<JSON
{
    "id": "com.lsd.tvf.web",
    "version": "$VER",
    "vendor": "L-S-D",
    "type": "native",
    "main": "bundle/run.sh",
    "title": "TVF WebKit",
    "icon": "icon.png",
    "visible": false
}
JSON
cd "$DIR"
# ares-package wants to see a terminal
script -qec "ares-package '$PKG' -o '$DIR'" /dev/null
rm -rf "$PKG"
ls -la "$DIR"/com.lsd.tvf.web_*.ipk
