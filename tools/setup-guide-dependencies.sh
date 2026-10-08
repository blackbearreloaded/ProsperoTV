#!/usr/bin/env bash
# ps5-native-app-boilerplate / ProsperoTV - Pinned streaming XML parser.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
target=${1:-host}
[[ $target == host || $target == ps5 ]] || exit 2
version=2.8.5
hash=1e727b8933ec51a77a9a9d9afcf8e688bce45d907c13e36ab7393fe36e703182
cache="$root/.deps/expat"
archive="$cache/expat-$version.tar.xz"
source="$cache/expat-$version"
build="$cache/$target-build"
prefix="$cache/$target"
stamp=$(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)
if [[ -f $prefix/.complete && $(<"$prefix/.complete") == "$stamp" ]]; then
    printf '%s\n' "$prefix"
    exit 0
fi
mkdir -p "$cache"
[[ -f $archive ]] || curl --fail --location --max-time 120 \
    "https://github.com/libexpat/libexpat/releases/download/R_2_8_5/expat-$version.tar.xz" -o "$archive"
printf '%s  %s\n' "$hash" "$archive" | sha256sum --check --strict >&2
[[ -f $source/CMakeLists.txt ]] || tar -xJf "$archive" -C "$cache"
flags=()
if [[ $target == ps5 ]]; then
    sdk="$root/.deps/native/ps5-payload-sdk"
    source "$sdk/toolchain/prospero.sh"
    flags+=(-DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_C_COMPILER="$sdk/bin/prospero-clang"
        -DCMAKE_AR="$sdk/bin/prospero-ar" -DCMAKE_RANLIB="$sdk/bin/prospero-ranlib"
        -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY -DEXPAT_WITH_GETRANDOM=OFF
        -DEXPAT_WITH_SYS_GETRANDOM=OFF -DEXPAT_WITH_GETENTROPY=OFF
        -DEXPAT_WITH_ARC4RANDOM=OFF -DEXPAT_WITH_ARC4RANDOM_BUF=OFF -DEXPAT_DEV_URANDOM=ON)
fi
cmake -S "$source" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_INSTALL_LIBDIR=lib -DEXPAT_SHARED_LIBS=OFF \
    -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF \
    -DEXPAT_BUILD_DOCS=OFF -DEXPAT_DTD=OFF -DEXPAT_GE=OFF "${flags[@]}" >&2
cmake --build "$build" -j4 >&2
DESTDIR= cmake --install "$build" >&2
cp "$source/COPYING" "$prefix/EXPAT-LICENSE.txt"
printf '%s\n' "$stamp" > "$prefix/.complete"
printf '%s\n' "$prefix"
