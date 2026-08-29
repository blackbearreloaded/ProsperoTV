# Architecture

## Project lineage and identity

psiptv is a native PS5 application built from
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate).
It adopts the application organization and lifecycle practices proven in
[PSRadio](https://github.com/blackbearreloaded/ps5-radio), especially the
native SDL/RmlUi shell, controller input, asynchronous network refresh,
`/download0` persistence, diagnostics, and deterministic teardown.

The application identity is defined in `sce_sys/param.json`:

| Field | Value |
| --- | --- |
| Display name | `psiptv` |
| Title ID | `PPSA88000` |
| Concept ID | `88000` |
| Content ID | `UP9000-PPSA88000_00-PS5IPTVAPP000001` |
| Content version | `01.000.001` |

The title metadata includes the high-resolution capability attribute used by
the native 4K presentation path.

## Component flow

```text
iptv-org index.m3u or custom M3U
              |
              v
      HTTP fetch + redirects
              |
              v
  bounded extended-M3U parser
              |
              v
  SQLite last-good catalog cache
              |
              v
 in-memory browse/search model
              |
              v
 HLS selection or direct stream
              |
              v
 MPEG-TS PAT/PMT/PES demux
       |                 |
       v                 v
 H.264/HEVC AU        AAC ADTS
       |                 |
       v                 v
   Videodec2          AudioDec2
       |
       v
 SDR NV12 AGC presenter -> VideoOut
```

VP9 currently joins this design at the coded-packet/Videodec2 boundary. No
WebM/Matroska or DASH demuxer connects catalog delivery to that boundary yet.

## Catalog and persistence

The built-in source is
`https://iptv-org.github.io/iptv/index.m3u`, published by
[iptv-org/iptv](https://github.com/iptv-org/iptv). The parser retains channel
identity, name, URL, logo URL, group, country, language, alternate URLs and
groups, and `#EXTVLCOPT` user-agent/referrer values. Duplicate records are
merged within bounded limits.

Startup is cache-first:

1. Read the selected source and its last good SQLite cache.
2. Populate the UI immediately when that cache is valid.
3. Fetch and parse the selected playlist on a background thread.
4. Write a complete staging database, run SQLite `quick_check`, then promote
   it to the live cache while retaining a recovery backup during replacement.
5. Keep the previous valid cache if download, parse, validation, or promotion
   fails.

The built-in and custom caches are deliberately separate:

| Source | Cache |
| --- | --- |
| iptv-org | `/download0/psiptv-catalog.sqlite3` |
| Custom M3U/M3U8 | `/download0/psiptv-custom-catalog.sqlite3` |

The schema has metadata, channels, alternate URL, and alternate group tables,
plus lookup indexes. SQLite uses an in-memory journal and temporary store for
cache generation. The database is a persistence and validation boundary; the
current UI loads its rows into an in-memory catalog and performs filtering
there.

Favorites, recent channels, custom-source configuration, source selection,
and playback receipts use separate versioned files under `/download0`.

## Network and playlist path

The HTTP layer supports HTTP(S), bounded redirects, deadlines, streaming
reads, and safe propagation of per-channel `User-Agent` and `Referer` values.
HLS master playlists are reduced to variants whose declared geometry is one of
720p, 1080p, 1440p, or 2160p and is within the configured bandwidth limit.

The media path currently accepts:

- direct MPEG-TS streams;
- HLS media playlists whose segments contain MPEG-TS;
- H.264 stream type `0x1b` or HEVC stream type `0x24`;
- optional AAC ADTS audio.

The MPEG-TS layer discovers PAT/PMT state, assembles bounded PES payloads,
extracts codec configuration from SPS data, emits complete Annex-B access
units, and preserves presentation timestamps for native pacing.

It does not currently accept fragmented MP4, WebM/Matroska, or DASH. The
stream codec enum and player adapter expose only H.264 and HEVC, which is the
specific reason VP9 Profile 0 is not yet reachable during normal channel
playback.

## Native video backend

The decoder owns distinct input and frame pools and validates every requested
codec/profile/geometry combination against a fixed mode table before opening
Videodec2. H.264 and HEVC use a one-frame decoder pipeline. VP9 Profile 0 uses
a depth-three pipeline.

VP9 coded packets are split using the trailing superframe index. Every coded
frame is submitted in packet order, including hidden frames. The fixed
uncompressed-header prefix is parsed so hidden frames are decoded without
being presented, while `show_frame` and `show_existing_frame` events remain in
submission order.

### Configured hardware modes

| Codec | Profile | Coded / visible geometry | Maximum configured level | Output pitch |
| --- | --- | --- | --- | --- |
| H.264 | Baseline, Main, High | 1280×720 / 1280×720 | 41 | 1280 |
| H.264 | Baseline, Main, High | 1920×1088 / 1920×1080 | 51 | 2048 |
| H.264 | Baseline, Main, High | 2560×1440 / 2560×1440 | 51 | 2560 |
| H.264 | Baseline, Main, High | 3840×2160 or 3840×2176 / 3840×2160 | 52 | 3840 |
| HEVC | Main, 8-bit 4:2:0 | 1280×720 / 1280×720 | 123 | 1280 |
| HEVC | Main, 8-bit 4:2:0 | 1920×1080 or 1920×1088 / 1920×1080 | 123 | 2048 |
| HEVC | Main, 8-bit 4:2:0 | 2560×1440 / 2560×1440 | 150 | 2560 |
| HEVC | Main, 8-bit 4:2:0 | 3840×2160 or 3840×2176 / 3840×2160 | 153 | 3840 |
| VP9 | Profile 0, 8-bit 4:2:0 | 1920×1080 / 1920×1080 | 41 | 2048 |
| VP9 | Profile 0, 8-bit 4:2:0 | 2560×1440 / 2560×1440 | 50 | 2560 |
| VP9 | Profile 0, 8-bit 4:2:0 | 3840×2160 / 3840×2160 | 51 | 3840 |

The level values above are the numeric values consumed by the current native
backend, not marketing labels inferred by the documentation.

VP9 Profile 2 is intentionally absent from the accepted codec/profile table.
Although Videodec2 can decode it on investigated hardware, Profile 2 produces
a low-aligned 10-bit 4:2:0 surface. The current AGC presenter consumes SDR
8-bit NV12, so accepting Profile 2 now would decode into a surface that cannot
be rendered correctly.

## Native output geometry

The presenter chooses one of two VideoOut surfaces:

| Visible source geometry | Output surface | Behavior |
| --- | --- | --- |
| Width ≤ 1920 and height ≤ 1080 | 1920×1080 | Render into the 1080p framebuffer |
| Width > 1920 or height > 1080 | 3840×2160 | Render into the 4K framebuffer |

Therefore 720p and 1080p use the 1080p output class, 1440p is bilinearly
scaled by AGC to the 4K surface, and 2160p maps to the 4K surface. Framebuffer
registration, viewport/scissor state, TV-safe inset, and allocation size are
recreated when the output class changes.

## Cleanup and diagnostics

Native decoder, audio, AGC, VideoOut, network, UI, and direct-memory resources
have explicit stop/drain/close paths. Playback writes a bounded diagnostic
receipt to `/download0/iptv-last-receipt.txt`, including codec, stream format,
decoded/presented counts, pipeline drain state, timing, and zero-copy pointer
observations. This receipt is intended to complement crash and kernel logs in
the project's investigation loop; it is not a substitute for hardware
acceptance testing.
