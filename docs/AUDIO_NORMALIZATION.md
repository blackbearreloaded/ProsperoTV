# Optional channel volume normalization

`Settings → AUDIO → Normalize channel volume` gradually levels decoded playback
volume. It is off by default and saved per console profile. Existing settings
files keep it off. The normal Volume control still applies afterward, including
mute. Interface sounds do not pass through this processor.

The processor runs on the common stereo S16, 48 kHz sink after downmixing and
resampling. It therefore covers native AAC and software audio paths without
changing codec selection or downmix coefficients.

## Control and limits

- K-weighted stereo energy uses the BS.1770 48 kHz filter coefficients.
- Up to thirty accepted 100 ms energy windows drive a rolling estimate; at least
  five accepted windows are needed before leveling begins.
- Target: -23 LUFS. Gain is bounded to [-18, +18] dB; upward changes are limited
  to 3 dB/s, downward changes to 12 dB/s.
- Windows below -50 LUFS are excluded from the program estimate. During quiet
  blocks, gain returns toward unity instead of rising. This cannot distinguish
  background noise above the threshold from intentional audio.
- A linked stereo block limiter uses the existing 256-frame output buffer as
  lookahead. It immediately reduces gain for peaks and releases over roughly
  250 ms. The ceiling is conservatively -1 dBFS **sample peak**, not dBTP.
- Channel/track changes, stream discontinuities, and toggling the feature reset
  history. Disabling the feature passes samples through unchanged.

This is a bounded playback leveler, not a certified integrated EBU R128 meter.
It does not guarantee identical subjective volume for every program, cannot
repair already-clipped source audio, and cannot recover absent audio tracks.
Boosting very quiet audio may still stop below the target because of the gain
cap. Very faint audio below the gate is deliberately not boosted.

## Diagnostics

Playback receipts add `audio_normalization_enabled`,
`audio_normalization_gain_millidb`, `audio_normalization_loudness_millilufs`, and
`audio_normalization_limited_blocks`. Loudness describes the input to the
normalizer (after downmix), not its output. `INT32_MIN` means the estimate is not
acquired. Values reflect the last submitted output block.

## Validation

Run `make test-audio-normalize` for sanitizer-backed behavioral checks of
bypass, silence, tiny signals, gain bounds, slew limits, loud transitions, linked
stereo limiting, and reset behavior. Run `make test-audio-normalize-ffmpeg` for
independent FFmpeg checks of K weighting at 100 Hz, 1 kHz, and 10 kHz, target
convergence, maximum boost, and bypass. The normal UI tests cover the toggle,
persistence, legacy defaults, and interactions with the Volume control.

Before release, build the OpenGL app with the existing Linux CI toolchain and
listen on PS5 with normalization off/on. Compare a loud channel, an ordinary
channel, and a consistently quiet channel; then test silence, a sudden loud
scene, mute, track switching, channel switching, and restart persistence. Keep
speaker volume moderate during the comparison. Host tests do not validate PS5
runtime linkage, CPU cost, device output, or subjective pumping/distortion.
