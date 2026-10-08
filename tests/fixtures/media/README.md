# Synthetic container fixtures

Generated from FFmpeg `testsrc2` and a 440 Hz sine tone. No provider material.
H.264/AAC files contain one second at 320×180, 25 fps; HEVC contains five
160×96 frames, without audio. The two MP4 layouts exercise a `moov` atom
before and after the media data; Matroska uses the same H.264/AAC packets.

`tools/make-media-fixtures.sh` generates these under ignored
`build/media-tests/fixtures/`, using the host FFmpeg CLI, libx264 and libx265.
The tests decode/remux them using the pinned production FFmpeg build.
Generation commands:

```sh
ffmpeg -f lavfi -i testsrc2=size=320x180:rate=25 -f lavfi -i sine=frequency=440:sample_rate=48000 -t 1 -c:v libx264 -threads 2 -profile:v high -pix_fmt yuv420p -c:a aac -b:a 64k h264-aac.mp4
ffmpeg -i h264-aac.mp4 -c copy h264-aac.mkv
ffmpeg -i h264-aac.mp4 -c copy -movflags +faststart h264-aac-fast.mp4
ffmpeg -f lavfi -i testsrc2=size=160x96:rate=10 -t 0.5 -c:v libx265 -x265-params pools=1:frame-threads=1 -an hevc.mp4
```
