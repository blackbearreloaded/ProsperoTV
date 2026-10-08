#!/usr/bin/env bash
# ProsperoTV - Build and run the PC tests of the new interface.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/run-tests.sh [gtest arguments]
#
# KIT the ps5-homebrew-ui checkout (default: fetched at its pinned commit by tools/fetch-kit.sh)
# TV  the ProsperoTV checkout      (default: the repository this folder is in)
#
# The tests need no OpenGL: the interface records draw lists and the tests
# read them. Everything runs under AddressSanitizer and UBSan.

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kit=$(bash "$root/tools/fetch-kit.sh")
tv=$(cd -- "${TV:-$root/..}" && pwd)
cxx=$(command -v "${HOST_CXX:-clang++}")
cache=$(command -v ccache || true)
gtest=$(bash "$tv/tools/setup-test-dependencies.sh" | tail -1)
expat=$(bash "$tv/tools/setup-guide-dependencies.sh" host | tail -1)
build="$root/build/tests"
mkdir -p "$build/obj"

sanitize="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
includes="-I$root/src -I$root/ps5/src -I$root/host -I$kit/src -I$kit/third_party -I$tv/include -I$tv/vendor/qrcodegen -I$expat/include"
{
    echo "rule cxx"
    echo "  command = $cache $cxx -std=c++20 -O1 $sanitize \$flags $includes -MD -MF \$out.d -c \$in -o \$out"
    echo "  depfile = \$out.d"
    echo "  deps = gcc"
    echo "  description = CXX \$in"
    echo "rule link"
    echo "  command = $cxx $sanitize \$in $expat/lib/libexpat.a -lpng -ljpeg -lz -lsqlite3 -lpthread -lm -o \$out"
    echo "  description = LINK \$out"
    objects=()
    edge() {
        local source=$1 flags=$2
        local object="$build/obj/$(echo "${source#/}" | tr '/' '_').o"
        echo "build $object: cxx $source"
        echo "  flags = $flags"
        objects+=("$object")
    }
    strict="-Wall -Wextra -Wpedantic -Werror"
    # The interface is built the way the console builds it.
    for source in "$root"/src/tv/*.cpp; do
        edge "$source" "$strict -fno-exceptions -fno-rtti"
    done
    edge "$root/host/host_platform.cpp" "$strict"
    edge "$root/host/host_preview.cpp" "$strict"
    # The tuning screen the player shows is plain CPU work: tested here too.
    for source in "$root"/ps5/src/tv_plate.cpp "$root"/ps5/src/tv_tuning.cpp; do
        edge "$source" "$strict"
    done
    for source in "$root"/tests/*.cpp; do
        edge "$source" "$strict -isystem $gtest/googletest/include"
    done
    for source in "$kit/host/platform_host.cpp"; do
        edge "$source" "-Wall -Wextra"
    done
    while IFS= read -r relative; do
        [[ -n $relative && $relative != gfx/gl_* && $relative != gfx/renderer.cpp &&
            $relative != gfx/backdrop.cpp && $relative != gfx/canvas.cpp ]] || continue
        edge "$kit/src/$relative" "-Wall -Wextra"
    done < "$root/tools/kit-sources.txt"
    for name in iptv_catalog iptv_http iptv_source_state iptv_store iptv_user_state iptv_xtream; do
        edge "$tv/src/$name.cpp" "-Wall -Wextra"
    done
    edge "$gtest/googletest/src/gtest-all.cc" "-isystem $gtest/googletest/include -I$gtest/googletest"
    edge "$gtest/googletest/src/gtest_main.cc" "-isystem $gtest/googletest/include -I$gtest/googletest"
    echo "build $build/tv_tests: link ${objects[*]}"
    echo "default $build/tv_tests"
} > "$build/build.ninja"

if ! ninja -j "${BUILD_JOBS:-4}" -C "$build" > "$build/build.log" 2>&1; then
    grep -E 'error|FAILED' -A12 "$build/build.log" | head -150 >&2
    exit 1
fi
grep -E 'warning' -A5 "$build/build.log" | head -60 >&2 || true

KIT_FONTS=$(bash "$root/tools/bake-fonts.sh" 2>/dev/null) ASAN_OPTIONS=detect_leaks=1 \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    "$build/tv_tests" "$@"
