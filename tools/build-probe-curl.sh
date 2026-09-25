#!/usr/bin/env bash
# ps5-native-app-boilerplate - Optional diagnostic curl build.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
# Diagnostic-only curl: keep threaded DNS, use its built-in polling fallback.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
sdk="$root/.deps/native/ps5-payload-sdk"
prefix="$root/.deps/pacbrew/v0.40.2/sysroot/user/homebrew"
archive="$root/.deps/curl-8.18.0.tar.xz"
source="$root/.deps/curl-8.18.0"
build="$root/build/curl-probe"
if [[ ! -f $archive ]]; then
    curl --fail --location --max-time 60 https://curl.se/download/curl-8.18.0.tar.xz -o "$archive"
fi
echo "40df79166e74aa20149365e11ee4c798a46ad57c34e4f68fd13100e2c9a91946  $archive" | sha256sum -c -
if [[ ! -d $source ]]; then
    tar -xJf "$archive" -C "$root/.deps"
fi
fresh=()
[[ -f $build/lib/libcurl.a ]] || fresh=(--fresh)
cmake "${fresh[@]}" -S "$source" -B "$build" -G Ninja \
    -DCMAKE_SYSTEM_NAME=FreeBSD -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_C_COMPILER=clang-18 -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
    -DCMAKE_C_FLAGS="-target x86_64-unknown-freebsd11 -isystem $sdk/target/include -fno-stack-protector -fno-plt -femulated-tls" \
    -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld -nostdlib -L$sdk/target/lib -Wl,--no-as-needed -lkernel -lSceLibcInternal -lc -lSceNet" \
    -DCMAKE_HAVE_LIBC_PTHREAD=1 \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON \
    -DBUILD_CURL_EXE=OFF -DBUILD_TESTING=OFF -DBUILD_LIBCURL_DOCS=OFF \
    -DBUILD_MISC_DOCS=OFF -DENABLE_CURL_MANUAL=OFF -DHTTP_ONLY=ON \
    -DCURL_DISABLE_SOCKETPAIR=ON -DENABLE_THREADED_RESOLVER=ON \
    -DCURL_USE_OPENSSL=ON -DOPENSSL_INCLUDE_DIR="$prefix/include" \
    -DOPENSSL_SSL_LIBRARY="$prefix/lib/libssl.a" -DOPENSSL_CRYPTO_LIBRARY="$prefix/lib/libcrypto.a" \
    -DCURL_USE_LIBPSL=OFF -DCURL_ZSTD=OFF -DCURL_BROTLI=OFF -DCURL_ZLIB=OFF \
    -DUSE_NGHTTP2=OFF -DUSE_LIBIDN2=OFF -DCURL_USE_LIBSSH2=OFF
cmake --build "$build" --target libcurl_static --parallel 8
if nm -u "$build/lib/libcurl.a" | grep -E '[[:space:]]Curl_(pipe|socketpair)$' >/dev/null; then
    echo 'Diagnostic curl unexpectedly requires wake-up descriptors' >&2
    exit 1
fi
