#!/usr/bin/env bash
# ProsperoTV - Container playback tests against the pinned production FFmpeg.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"
build="$root/build/media-tests"
mkdir -p "$build"
bash tools/make-media-fixtures.sh
if ! bash tools/setup-audio-dependencies.sh host > "$build/dependencies.log" 2>&1; then
    tail -60 "$build/dependencies.log" >&2
    exit 1
fi
ffmpeg="$root/.deps/ffmpeg-host/root"
gtest=$(bash tools/setup-test-dependencies.sh | tail -1)
"${HOST_CXX:-clang++}" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Wall -Wextra -Wpedantic -Werror -pthread -Iinclude -I"$ffmpeg/include" \
    -isystem "$gtest/googletest/include" -I"$gtest/googletest" \
    tests/test_iptv_media.cpp src/iptv_media.cpp src/iptv_stream.cpp \
    "$gtest/googletest/src/gtest-all.cc" "$gtest/googletest/src/gtest_main.cc" \
    "$ffmpeg/lib/libavformat.a" "$ffmpeg/lib/libavcodec.a" "$ffmpeg/lib/libavutil.a" \
    -lm -o "$build/media_tests"
ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 "$build/media_tests" "$@"
