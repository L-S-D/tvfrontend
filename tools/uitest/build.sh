#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# PC build of the UI (LVGL + src/ui) for uitest: renders into a
# buffer and writes PNGs - layout work without a TV.
#   export TVF_KODI=<tv>:8080   # TVF's Kodi JSON-RPC (running TVF) for the test data
#   ./build.sh && python3 -I mkgrid.py 4 > grid.json && python3 -I mkrecs.py > recs.json
#   ./build/uitest ../../res grid.json shots/a open shot DOWN YELLOW shot recs shot
set -e
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../.." && pwd)
B=$H/build
mkdir -p "$B/lvgl" "$H/shots"
CF="-O1 -g -DLV_CONF_INCLUDE_SIMPLE -I$R/src/ui -I$R/third_party/lvgl -I$R/third_party/lvgl/src/libs/lodepng -I$R/third_party/cJSON -I$R/src/core -I$R/src/net -I$R/third_party/stb"
if [ ! -f "$B/liblvgl.a" ]; then
  find "$R/third_party/lvgl/src" -name '*.c' | sort > "$B/lvgl.list"
  xargs -P 16 -I{} sh -c 'o=$0/lvgl/$(echo "$1" | md5sum | cut -c1-12).o; gcc $2 -w -c "$1" -o "$o"' "$B" {} "$CF" < "$B/lvgl.list"
  ar rcs "$B/liblvgl.a" "$B"/lvgl/*.o
fi
gcc $CF -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined -o "$B/uitest" "$H/uitest.c" \
  "$R/src/ui/epg.c" "$R/src/ui/choice.c" "$R/src/ui/recdlg.c" "$R/src/ui/recbrowser.c" "$R/src/ui/osd.c" "$R/src/ui/rxview.c" "$R/src/ui/settings.c" "$R/src/ui/info.c" \
  "$R/src/ui/ui.c" "$R/src/ui/imgtrim.c" "$R/src/ui/keys.c" "$R/src/core/log.c" "$R/src/core/prefs.c" "$R/src/core/i18n.c" "$R/src/net/http.c" "$R/src/net/md5.c" \
  "$R/third_party/cJSON/cJSON.c" "$B/liblvgl.a" -lssl -lcrypto -lpthread -lm
echo "built: $B/uitest"
