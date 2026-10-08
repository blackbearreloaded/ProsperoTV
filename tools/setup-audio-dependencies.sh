#!/usr/bin/env bash
# ProsperoTV - Pinned FFmpeg audio decoders and MP4/Matroska container reader.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
version=8.0.1
hash=05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41
archive="$root/.deps/ffmpeg-$version.tar.xz"
source="$root/.deps/ffmpeg-$version"
target=${1:-ps5}
[[ $target == ps5 || $target == host ]] || { echo 'expected ps5 or host' >&2; exit 2; }
suffix=audio
[[ $target == ps5 ]] || suffix=host
build="$root/.deps/ffmpeg-$suffix/build"
prefix="$root/.deps/ffmpeg-$suffix/root"
stamp=$(sha256sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)
if [[ -f $prefix/.complete && $(<"$prefix/.complete") == "$stamp" && -f $prefix/lib/libavformat.a ]]; then
    exit 0
fi
mkdir -p "$root/.deps" "$build" "$prefix"
if [[ ! -f $archive ]]; then
    curl --fail --location --max-time 120 "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz" -o "$archive"
fi
printf '%s  %s\n' "$hash" "$archive" | sha256sum --check --strict
[[ -f $source/configure ]] || tar -xJf "$archive" -C "$root/.deps"
cross=()
if [[ $target == ps5 ]]; then
    sdk="$root/.deps/native/ps5-payload-sdk"
    source "$sdk/toolchain/prospero.sh"
    unset DESTDIR PREFIX
    cross=(--target-os=freebsd --arch=x86_64 --enable-cross-compile
        --cc="$sdk/bin/prospero-clang" --ar="$sdk/bin/prospero-ar"
        --ranlib="$sdk/bin/prospero-ranlib")
fi
cd "$build"
"$source/configure" --prefix="$prefix" "${cross[@]}" --disable-autodetect --disable-everything \
    --disable-programs --disable-doc --disable-network --disable-avdevice \
    --disable-avfilter --disable-swscale --disable-shared --enable-static \
    --disable-pthreads --disable-w32threads --disable-os2threads --disable-x86asm \
    --enable-avcodec --enable-avutil --enable-swresample --enable-avformat \
    --enable-demuxer=mov,matroska --enable-muxer=mpegts \
    --enable-parser=h264,hevc,aac,aac_latm,ac3,mpegaudio \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb \
    --enable-decoder=aac,aac_latm,ac3,eac3,mp2,mp3,h264,hevc
# The SDK probe linker permits unresolved imports. PS5 exports gmtime, not
# gmtime_r; select FFmpeg's own portability fallback instead of a bogus import.
if [[ $target == ps5 ]]; then
    sed -i 's/^#define HAVE_GMTIME_R 1$/#define HAVE_GMTIME_R 0/' config.h
fi
make -j"${BUILD_JOBS:-4}"
make install
printf '%s\n' "$stamp" > "$prefix/.complete"
