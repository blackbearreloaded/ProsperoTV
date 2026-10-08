#!/usr/bin/env bash
# ProsperoTV - Build the interface for the PC and render its walk to PNG files.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: tools/host-snapshots.sh [output dir] [width height]
#
# KIT the ps5-homebrew-ui checkout (default: fetched at its pinned commit by tools/fetch-kit.sh)
# TV  the ProsperoTV checkout      (default: the repository this folder is in)

set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kit=$(bash "$root/tools/fetch-kit.sh")
tv=$(cd -- "${TV:-$root/..}" && pwd)
cxx=$(command -v "${HOST_CXX:-clang++}")
cache=$(command -v ccache || true)
expat=$(bash "$tv/tools/setup-guide-dependencies.sh" host | tail -1)
# HOST_SANITIZE=1 runs the walk under AddressSanitizer and UBSan.
sanitize=""
build="$root/build/host"
if [[ ${HOST_SANITIZE:-0} == 1 ]]; then
    sanitize="-fsanitize=address,undefined -fno-omit-frame-pointer -g"
    build="$root/build/host-sanitize"
fi
mkdir -p "$build/obj"

includes="-I$root/src -I$root/host -I$kit/src -I$kit/third_party -I$tv/include -I$tv/vendor/qrcodegen -I$expat/include"
{
    echo "rule cxx"
    echo "  command = $cache $cxx -std=c++20 -O2 -Wall -Wextra $sanitize \$flags -DGL_GLEXT_PROTOTYPES=1 $includes -MD -MF \$out.d -c \$in -o \$out"
    echo "  depfile = \$out.d"
    echo "  deps = gcc"
    echo "  description = CXX \$in"
    echo "rule link"
    echo "  command = $cxx $sanitize \$in $expat/lib/libexpat.a -lpng -ljpeg -lz -lEGL -lGL -lsqlite3 -lpthread -lm -o \$out"
    echo "  description = LINK \$out"
    objects=()
    edge() {
        local source=$1 flags=$2
        local object="$build/obj/$(echo "${source#/}" | tr '/' '_').o"
        echo "build $object: cxx $source"
        echo "  flags = $flags"
        objects+=("$object")
    }
    # The interface itself is built the way the console builds it.
    for source in "$root"/src/tv/*.cpp; do
        edge "$source" "-Werror -fno-exceptions -fno-rtti"
    done
    for source in "$root"/host/*.cpp "$kit/host/platform_host.cpp"; do
        edge "$source" ""
    done
    while IFS= read -r relative; do
        [[ -n $relative ]] || continue
        edge "$kit/src/$relative" ""
    done < "$root/tools/kit-sources.txt"
    for name in iptv_catalog iptv_http iptv_source_state iptv_store iptv_user_state iptv_xtream; do
        edge "$tv/src/$name.cpp" ""
    done
    echo "build $build/tv_snapshots: link ${objects[*]}"
    echo "default $build/tv_snapshots"
} > "$build/build.ninja"

if ! ninja -j "${BUILD_JOBS:-4}" -C "$build" > "$build/build.log" 2>&1; then
    grep -E 'error|FAILED' -A8 "$build/build.log" | head -150 >&2
    exit 1
fi
grep -E 'warning' -A5 "$build/build.log" | head -60 >&2 || true

fonts=$(bash "$root/tools/bake-fonts.sh" 2>/dev/null)
output=${1:-"$root/build/snapshots"}
mkdir -p "$output"
cp "$root/ps5/sce_sys/icon0.png" "$output/test-logo.png"
playlist="$root/build/sample.m3u"
[[ -f $playlist ]] || python3 "$root/tools/make-sample-playlist.py" "$playlist"
# The software GL driver keeps memory until exit; only the app's own errors count.
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
    EGL_PLATFORM=surfaceless LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
    "$build/tv_snapshots" "$fonts" "$playlist" "$output" "${@:2}"
