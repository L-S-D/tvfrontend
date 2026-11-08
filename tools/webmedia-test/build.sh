#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Build the test bench for the media server (src/app/webmedia.c) on the host and
# run it against WebKit under qemu:
#   sh tools/webmedia-test/build.sh            -> build/webmedia-test/webmedia-test
#   build/webmedia-test/webmedia-test 120 &    (Socket @tvf-media-test, 120 s)
# then in wpe-webos/sdk: media bundle under qemu with TVF_MEDIA_SOCK=@tvf-media-test
# (tests/mse/page.sh, URL mse.html?pause&seekplay&noseek).
# EME: fake_cdm.c instead of the TV's CDMs (readable challenge/license, decrypt appends
# "FAKESVP!"); counterpart wpe-webos sdk/tests/eme (OpenCDM client).
set -e
cd "$(dirname "$0")/../.."
mkdir -p build/webmedia-test
gcc -O1 -g -Wall -Wextra -Wno-unused-parameter -fsanitize=address,undefined \
    -Isrc/app -Isrc/core -Isrc/player -Ithird_party/cJSON \
    -o build/webmedia-test/webmedia-test tools/webmedia-test/main.c tools/webmedia-test/fake_smp.c \
    tools/webmedia-test/fake_cdm.c src/app/webmedia.c src/app/webdrm.c src/core/log.c third_party/cJSON/cJSON.c \
    -lpthread
echo "built: build/webmedia-test/webmedia-test"
