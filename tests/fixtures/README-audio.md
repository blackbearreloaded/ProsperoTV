# Generated audio regression fixtures

These short test tones contain no broadcast material. Generated with FFmpeg 8.0.1;
the synthetic signals are dedicated to the public domain under CC0-1.0.

- `aac-stereo.aac`, `aac-latm.bin`: 997 Hz, 48 kHz stereo, 0.15 seconds,
  encoded with the native AAC encoder in ADTS and LATM respectively.
- `aac-surround.bin`, `ac3-surround.bin`, `eac3-surround.bin`: 48 kHz 5.1,
  0.15 seconds, 192 kbps, with a 997 Hz signal **only in the center channel**.
  This verifies that stereo downmix retains dialogue rather than merely taking
  the first two source channels.

The decoder check verifies nonzero left/right output and decoding after reset.
AAC Main framing is also covered by the transport tests; actual AAC Main
playback was checked with a private source sample, not distributed here.
