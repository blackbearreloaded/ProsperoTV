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
stamp=$(cat "${BASH_SOURCE[0]}" "$root/tools/patch-ffmpeg-hls.py" "$root/tools/patch-ffmpeg-color.py" | sha256sum | cut -d' ' -f1)
if [[ -f $prefix/.complete && $(<"$prefix/.complete") == "$stamp" && -f $prefix/lib/libavformat.a ]]; then
    exit 0
fi
mkdir -p "$root/.deps" "$build" "$prefix"
if [[ ! -f $archive ]]; then
    curl --fail --location --max-time 120 "https://ffmpeg.org/releases/ffmpeg-$version.tar.xz" -o "$archive"
fi
printf '%s  %s\n' "$hash" "$archive" | sha256sum --check --strict
[[ -f $source/configure ]] || tar -xJf "$archive" -C "$root/.deps"
python3 "$root/tools/patch-ffmpeg-hls.py" "$source"
python3 "$root/tools/patch-ffmpeg-color.py" "$source"
cross=()
if [[ $target == ps5 ]]; then
    sdk="$root/.deps/native/ps5-payload-sdk"
    source "$sdk/toolchain/prospero.sh"
    unset DESTDIR PREFIX
    ports=$(bash "$root/tools/setup-pacbrew-dependencies.sh" --all)
    export PKG_CONFIG_DIR=
    export PKG_CONFIG_SYSROOT_DIR="$ports"
    export PKG_CONFIG_LIBDIR="$ports/user/homebrew/lib/pkgconfig"
    export PKG_CONFIG_PATH="$ports/user/homebrew/libdata/pkgconfig"
    cross=(--target-os=freebsd --arch=x86_64 --enable-cross-compile --pkg-config=pkg-config
        --cc="$sdk/bin/prospero-clang" --ar="$sdk/bin/prospero-ar"
        --ranlib="$sdk/bin/prospero-ranlib")
fi
cd "$build"
"$source/configure" --prefix="$prefix" "${cross[@]}" --disable-autodetect --disable-everything \
    --disable-programs --disable-doc --enable-network --disable-avdevice \
    --enable-openssl --enable-protocol=http,https,udp --pkg-config-flags=--static \
    --disable-avfilter --disable-swscale --disable-shared --enable-static \
    --enable-pthreads --disable-w32threads --disable-os2threads --disable-x86asm \
    --enable-avcodec --enable-avutil --enable-swresample --enable-avformat \
    --enable-demuxer=mov,matroska,hls,webvtt --enable-muxer=mpegts \
    --enable-parser=h264,hevc,aac,aac_latm,ac3,mpegaudio \
    --enable-bsf=h264_mp4toannexb,hevc_mp4toannexb \
    --enable-decoder=aac,aac_latm,ac3,eac3,mp2,mp3,h264,hevc,text,subrip,ass,movtext,webvtt,dvbsub,dvdsub,pgssub
# The SDK probe linker permits unresolved imports. PS5 exports gmtime, not
# gmtime_r; select FFmpeg's own portability fallback instead of a bogus import.
if [[ $target == ps5 ]]; then
    sed -i 's/^#define HAVE_GMTIME_R 1$/#define HAVE_GMTIME_R 0/' config.h
    # Thread synchronization is required even with one decoding thread per
    # context. Optional FreeBSD thread naming functions are not PS5 imports.
    sed -i -E 's/^(#define HAVE_PTHREAD_SET_?NAME_NP) 1$/\1 0/' config.h
fi
# USE_CCACHE=1 compiles through ccache when it is installed. configure keeps the
# plain compiler: its probes are throwaway files that would only fill the cache.
compile=()
if [[ ${USE_CCACHE:-0} != 0 ]] && command -v ccache >/dev/null; then
    compiler=$(sed -n 's/^CC=//p' ffbuild/config.mak)
    compile=(CC="ccache $compiler")
    # The SDK's compiler is a script, so ccache is told which Clang is behind it.
    if [[ $target == ps5 ]]; then
        export CCACHE_COMPILERCHECK="string:$("$compiler" --version | sed -n 1p)"
    fi
fi
make -j"${BUILD_JOBS:-4}" "${compile[@]}"
make install
printf '%s\n' "$stamp" > "$prefix/.complete"
