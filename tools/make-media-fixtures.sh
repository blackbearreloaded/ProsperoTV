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
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=320x180:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 1 -c:v libx264 -threads 2 -profile:v high -pix_fmt yuv420p -x264-params ref=2 -c:a aac -b:a 64k "$output/h264-config.mp4"
ffmpeg -hide_banner -loglevel error -y -i "$output/h264-aac.mp4" -c copy "$output/h264-aac.mkv"
ffmpeg -hide_banner -loglevel error -y -i "$output/h264-aac.mp4" -c copy -movflags +faststart "$output/h264-aac-fast.mp4"
ffmpeg -hide_banner -loglevel error -y -i "$output/h264-aac.mp4" -f lavfi -i sine=frequency=880:sample_rate=48000 -t 1 -map 0:v -map 0:a -map 1:a -c:v copy -c:a aac -b:a 64k -metadata:s:a:0 language=eng -metadata:s:a:1 language=spa "$output/two-audio.mp4"
ffmpeg -hide_banner -loglevel error -y -i "$output/two-audio.mp4" -map 0 -c copy "$output/two-audio.mkv"
for format in mpegts fmp4 ranged; do
    dir="$output/hls-$format"
    mkdir -p "$dir"
    for track in video english spanish; do
        map=0:v:0
        [[ $track != english ]] || map=0:a:0
        [[ $track != spanish ]] || map=0:a:1
        segment_type=$format
        segment_options=()
        if [[ $format == ranged ]]; then
            segment_type=fmp4
            segment_options=(-hls_flags single_file -hls_segment_filename "$dir/$track-stream.mp4")
        fi
        ffmpeg -hide_banner -loglevel error -y -i "$output/two-audio.mp4" -map "$map" -c copy \
            -hls_time 0.4 -hls_list_size 0 -hls_segment_type "$segment_type" "${segment_options[@]}" \
            -hls_fmp4_init_filename "$track-init.mp4" "$dir/$track.m3u8"
    done
    cat > "$dir/master.m3u8" <<'MASTER'
#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="sound",NAME="English",LANGUAGE="eng",DEFAULT=YES,AUTOSELECT=YES,URI="english.m3u8"
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="sound",NAME="Español",LANGUAGE="spa",AUTOSELECT=YES,URI="spanish.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=500000,AUDIO="sound"
video.m3u8
MASTER
    first_pts=$(ffprobe -v error -select_streams v:0 -show_entries stream=start_time \
        -of default=nw=1:nk=1 "$dir/video.m3u8" | awk '{printf "%.0f", $1 * 90000; exit}')
    for language in en es; do
        first='Hello, <i>world</i>!'
        second='Second line'
        if [[ $language == es ]]; then first='Hola, mundo!'; second='Otra línea'; fi
        cat > "$dir/$language-0.vtt" <<CAPTIONS
WEBVTT
X-TIMESTAMP-MAP=LOCAL:00:00:10.000,MPEGTS:$first_pts

first
00:00:10.200 --> 00:00:10.600
$first
CAPTIONS
        cp "$dir/$language-0.vtt" "$dir/$language-1.vtt"
        cat >> "$dir/$language-1.vtt" <<CAPTIONS

second
00:00:10.650 --> 00:00:10.900
$second
CAPTIONS
        cat > "$dir/$language.m3u8" <<CAPTIONS
#EXTM3U
#EXT-X-TARGETDURATION:1
#EXTINF:0.5,
$language-0.vtt
#EXTINF:0.5,
$language-1.vtt
#EXT-X-ENDLIST
CAPTIONS
    done
    cat > "$dir/subtitles-master.m3u8" <<'MASTER'
#EXTM3U
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="sound",NAME="English",LANGUAGE="en-US",DEFAULT=YES,AUTOSELECT=YES,URI="english.m3u8"
#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID="sound",NAME="Español",LANGUAGE="es",AUTOSELECT=YES,URI="spanish.m3u8",CHARACTERISTICS="public.accessibility.describes-video"
#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="captions",NAME="English CC",LANGUAGE="eng",URI="en.m3u8",CHARACTERISTICS="public.accessibility.describes-music-and-sound"
#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="captions",NAME="Español",LANGUAGE="spa",URI="es.m3u8",FORCED=YES
#EXT-X-STREAM-INF:BANDWIDTH=500000,AUDIO="sound",SUBTITLES="captions"
video.m3u8
MASTER
done
dir="$output/hls-reset"
mkdir -p "$dir"
for epoch in old new; do
    offset=20
    [[ $epoch != new ]] || offset=0
    ffmpeg -hide_banner -loglevel error -y -i "$output/h264-aac.mp4" -c copy \
        -output_ts_offset "$offset" "$dir/$epoch.ts"
    first_pts=$(ffprobe -v error -select_streams v:0 -show_entries stream=start_time \
        -of default=nw=1:nk=1 "$dir/$epoch.ts" | awk '{printf "%.0f", $1 * 90000; exit}')
    cat > "$dir/$epoch.vtt" <<CAPTIONS
WEBVTT
X-TIMESTAMP-MAP=LOCAL:00:00:10.000,MPEGTS:$first_pts

00:00:10.200 --> 00:00:10.600
$epoch timeline
CAPTIONS
done
for extension in ts vtt; do
    cat > "$dir/$extension.m3u8" <<PLAYLIST
#EXTM3U
#EXT-X-TARGETDURATION:1
#EXT-X-MEDIA-SEQUENCE:0
#EXTINF:1,
old.$extension
#EXT-X-DISCONTINUITY
#EXTINF:1,
new.$extension
#EXT-X-ENDLIST
PLAYLIST
done
cat > "$dir/master.m3u8" <<'MASTER'
#EXTM3U
#EXT-X-MEDIA:TYPE=SUBTITLES,GROUP-ID="captions",NAME="English",LANGUAGE="eng",URI="vtt.m3u8"
#EXT-X-STREAM-INF:BANDWIDTH=500000,SUBTITLES="captions"
ts.m3u8
MASTER
cat > "$output/english.srt" <<'CAPTIONS'
1
00:00:00,200 --> 00:00:00,600
Hello, <i>world</i>!

2
00:00:00,650 --> 00:00:00,900
Second line
CAPTIONS
cat > "$output/spanish.srt" <<'CAPTIONS'
1
00:00:00,200 --> 00:00:00,600
Hola, mundo!

2
00:00:00,650 --> 00:00:00,900
Otra línea
CAPTIONS
ffmpeg -hide_banner -loglevel error -y -i "$output/h264-aac.mp4" -i "$output/english.srt" -i "$output/spanish.srt" -map 0 -map 1 -map 2 -c copy -c:s mov_text -metadata:s:s:0 language=eng -metadata:s:s:1 language=spa "$output/subtitles.mp4"
ffmpeg -hide_banner -loglevel error -y -i "$output/h264-aac.mp4" -i "$output/english.srt" -i "$output/spanish.srt" -map 0 -map 1 -map 2 -c copy -c:s srt -metadata:s:s:0 language=eng -metadata:s:s:1 language=spa "$output/subtitles.mkv"
# A normal multilingual movie can have more than 32 streams in its container.
for count in 40 128; do
    maps=(-map 0:v -map 0:a)
    for ((i=0; i<count; ++i)); do maps+=(-map 0:s:0); done
    ffmpeg -hide_banner -loglevel error -y -i "$output/subtitles.mkv" "${maps[@]}" \
        -c copy "$output/many-subtitles-$count.mkv"
done
ffmpeg -hide_banner -loglevel error -y -i "$output/many-subtitles-40.mkv" -map 0 \
    -c copy -c:s mov_text "$output/many-subtitles-40.mp4"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=160x96:rate=10 -t 0.5 -c:v libx265 -x265-params pools=1:frame-threads=1:log-level=error -an "$output/hevc.mp4"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=160x96:rate=10 -t 0.5 -c:v libx265 -x265-params pools=1:frame-threads=1:log-level=error:sao=0 -an "$output/hevc-config.mp4"
ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=160x96:rate=10 -t 0.5 -c:v libx265 -x265-params pools=1:frame-threads=1:log-level=error:repeat-headers=1 -an "$output/hevc-inband.mkv"
for order in 0 1 2; do
    field_options=()
    rate=25
    if [[ $order != 0 ]]; then
        rate=50
        mode=interleave_top
        dominance=tff
        if [[ $order == 2 ]]; then mode=interleave_bottom; dominance=bff; fi
        field_options=(-vf "tinterlace=$mode" -flags +ilme+ildct)
    fi
    params=aud=1:keyint=25
    [[ $order == 0 ]] || params+=:$dominance=1
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i "testsrc2=size=160x96:rate=$rate" \
        -t 1 -an "${field_options[@]}" -c:v libx264 -threads 2 -x264-params "$params" \
        -map 0:v -f tee "[f=h264]$output/fields-$order.h264|[f=mpegts]$output/fields-$order.ts"
done
for color in pq sdr full hlg unknown; do
    signaling=(-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range tv)
    vui=colorprim=9:transfer=16:colormatrix=9:range=limited
    case "$color" in
        sdr) signaling=(-color_primaries bt709 -color_trc bt709 -colorspace bt709 -color_range tv); vui=colorprim=1:transfer=1:colormatrix=1:range=limited ;;
        full) signaling=(-color_primaries bt2020 -color_trc smpte2084 -colorspace bt2020nc -color_range pc); vui=colorprim=9:transfer=16:colormatrix=9:range=full ;;
        hlg) signaling=(-color_primaries bt2020 -color_trc arib-std-b67 -colorspace bt2020nc -color_range tv); vui=colorprim=9:transfer=18:colormatrix=9:range=limited ;;
        unknown) signaling=(); vui=colorprim=2:transfer=2:colormatrix=2 ;;
    esac
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i testsrc2=size=160x96:rate=1 \
        -frames:v 1 -pix_fmt yuv420p10le -c:v libx265 -threads 1 \
        -x265-params "pools=1:frame-threads=1:log-level=error:repeat-headers=1:$vui" \
        "${signaling[@]}" -f hevc "$output/color-$color.hevc"
done
printf '%s\n' "$stamp" > "$output/.complete"
