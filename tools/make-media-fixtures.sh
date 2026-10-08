#!/usr/bin/env bash
# ProsperoTV - Synthetic container samples; generated media stays in build/.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
output="$root/build/media-tests/fixtures"
stamp=$(sha256sum "${BASH_SOURCE[0]}" | cut -d ' ' -f1)
if [[ -f $output/.complete && $(cat "$output/.complete") == "$stamp" ]]; then
    exit 0
fi
command -v ffmpeg >/dev/null || { echo 'Install the FFmpeg command-line tools to generate test fixtures.' >&2; exit 1; }
mkdir -p "$output"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x180:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 1 -c:v libx264 -threads 2 -profile:v high -pix_fmt yuv420p -c:a aac -b:a 64k "$output/h264-aac.mp4"
ffmpeg -hide_banner -loglevel error -y -i "$output/h264-aac.mp4" -c copy "$output/h264-aac.mkv"
ffmpeg -hide_banner -loglevel error -y -i "$output/h264-aac.mp4" -c copy -movflags +faststart "$output/h264-aac-fast.mp4"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=160x96:rate=10 -t 0.5 -c:v libx265 -x265-params pools=1:frame-threads=1:log-level=error -an "$output/hevc.mp4"
printf '%s\n' "$stamp" > "$output/.complete"
