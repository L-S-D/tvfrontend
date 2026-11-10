cmake_minimum_required(VERSION 3.13)
project(tvf C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Hardening: stack protector and the glibc checks (_FORTIFY_SOURCE; the
# SDK glibc 2.12 has all _chk functions, the TVs have newer ones). Source paths in
# the binary are relative to the project. Deliberately without PIE: the crash
# reporter logs pc/lr, which can then be resolved directly with addr2line.
add_compile_options(-fstack-protector-strong -ffile-prefix-map=${CMAKE_SOURCE_DIR}=.)
add_compile_definitions(_FORTIFY_SOURCE=2)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(PLAYERAPIS REQUIRED libplayerAPIs)

# LVGL (submodule third_party/lvgl, v9.5.0): only the C sources, lv_conf.h in src/ui
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/third_party/lvgl/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl PUBLIC ${CMAKE_SOURCE_DIR}/third_party/lvgl ${CMAKE_SOURCE_DIR}/src/ui)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE=1)
target_compile_options(lvgl PRIVATE -O2 -w)

add_library(cjson STATIC third_party/cJSON/cJSON.c)
target_include_directories(cjson PUBLIC third_party/cJSON)
target_compile_options(cjson PRIVATE -w)

add_executable(tvf
    src/app/main.c
    src/core/log.c
    src/net/http.c
    src/net/md5.c
    src/player/player.c
    src/player/hls.c
    src/player/ts.c
    src/player/mpa_pcm.c
    src/player/smp_api.cpp
    src/ui/ui.c src/ui/ui_sdl.c src/ui/epg.c src/ui/keys.c src/app/keys_sdl.c
    src/ui/osd.c src/ui/rxview.c src/ui/info.c src/ui/settings.c src/ui/choice.c src/ui/recdlg.c src/ui/recbrowser.c src/core/prefs.c src/core/i18n.c src/app/debugctl.c
    src/app/recindex.c src/app/recplay.c src/ui/ttx.c src/ui/subs.c src/net/fetch.c src/app/tshift.c
    src/app/sidedata.c src/core/ttxdec.c src/core/dvbsubdec.c
    src/app/luna.c src/app/hbbtv.c src/app/tvsrc.c src/app/tvrec.c src/app/kodi.c src/net/mdns.c src/app/tvhstat.c src/net/htsp.c src/net/htsmsg.c src/net/sha1.c
    src/ui/imgtrim.c src/app/webr.c src/app/hbbweb.c src/app/hbbmedia.c src/app/hbbdrm.c
    src/app/egress.c src/net/relay.c src/app/webmedia.c src/app/webdrm.c src/app/cdm.cpp src/app/cdmtest.c)
target_include_directories(tvf PRIVATE src/app src/core src/net src/player src/ui third_party/minimp3 third_party/stb third_party/lvgl/src/libs/lodepng third_party/cdm
    ${SDL2_INCLUDE_DIRS} ${PLAYERAPIS_INCLUDE_DIRS})
# unwind-tables: backtrace of the crash reporter on ARM
target_compile_options(tvf PRIVATE -Wall -Wextra -funwind-tables)
target_link_directories(tvf PRIVATE ${PLAYERAPIS_LIBRARY_DIRS})
# dl: libAcbAPI (webOS 4) is only loaded on demand
# https (media libraries): the SDK's OpenSSL statically - the TV has its own
# version; the symbols stay inside tvf (--exclude-libs)
find_library(SSL_STATIC NAMES libssl.a REQUIRED)
find_library(CRYPTO_STATIC NAMES libcrypto.a REQUIRED)
target_link_libraries(tvf lvgl cjson ${SDL2_LIBRARIES} ${PLAYERAPIS_LIBRARIES} ${SSL_STATIC} ${CRYPTO_STATIC} pthread m ${CMAKE_DL_LIBS})
target_link_options(tvf PRIVATE "-Wl,--exclude-libs,libssl.a:libcrypto.a")
# libstdc++ statically: the one C++ file only needs std::string; this way there
# are no GLIBCXX requirements on the firmware
target_link_options(tvf PRIVATE -static-libstdc++ -static-libgcc)

# Default for the tvh server as long as none is set in the settings ("" = none,
# then TVF points the way to the settings). build.sh takes it from local.env.
set(TVF_DEFAULT_TVH_HOST "" CACHE STRING "Default tvh server address")
target_compile_definitions(tvf PRIVATE TVF_DEFAULT_TVH_HOST="${TVF_DEFAULT_TVH_HOST}")

# Details a tvh without the HTSP extensions leaves out of the stream list (teletext subtitle
# pages, component tags, subtitling types, initial teletext page, audio description and receiver
# mix) are read from the PMT of the channel: once per channel the app opens the HTTP stream (pass
# profile) for a moment (src/app/pmtinfo.c). OFF: only what HTSP delivers; saves the extra stream
# and some code, and the subtitle pages of a plain tvh are missing then.
option(TVF_PMT_FALLBACK "Read missing stream details from the PMT over HTTP" ON)
if(TVF_PMT_FALLBACK)
  target_sources(tvf PRIVATE src/app/pmtinfo.c)
  target_compile_definitions(tvf PRIVATE TVF_PMT_FALLBACK=1)
else()
  target_compile_definitions(tvf PRIVATE TVF_PMT_FALLBACK=0)
endif()

# Diagnostics: PmLog of the DRM modules (TV CDMs) and failed /dev accesses
# into the log (src/app/drmdebug.c) - not for everyday use
option(TVF_DRM_DEBUG "PmLog of the DRM modules into the TVF log" OFF)
if(TVF_DRM_DEBUG)
  target_sources(tvf PRIVATE src/app/drmdebug.c)
  target_link_options(tvf PRIVATE -Wl,--export-dynamic-symbol=PmLogGetContext -Wl,--export-dynamic-symbol=_PmLogMsgKV
                      -Wl,--export-dynamic-symbol=open -Wl,--export-dynamic-symbol=open64)
endif()
