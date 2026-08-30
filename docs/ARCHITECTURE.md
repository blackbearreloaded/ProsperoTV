# Architecture

## Project lineage and identity

ProsperoTV is a native PS5 application built from
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate).
It adopts the application organization and lifecycle practices proven in
[PSRadio](https://github.com/blackbearreloaded/ps5-radio), especially the
native SDL/RmlUi shell, controller input, asynchronous network refresh,
`/download0` persistence, diagnostics, and deterministic teardown.

The application identity is defined in `sce_sys/param.json`:

| Field | Value |
| --- | --- |
| Display name | `ProsperoTV` |
| Title ID | `PPSA99003` |
| Concept ID | `99003` |
| Content ID | `UP9000-PPSA99003_00-PROSPEROTV000001` |
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
 HLS or direct MPEG-TS       direct WebM
              |                  |
              v                  v
 MPEG-TS PAT/PMT/PES demux   WebM blocks
       |                 |        |
       v                 v        v
 H.264/HEVC AU        AAC ADTS   VP9 AU
       |                 |        |
       +--------+        v        |
                v     AudioDec2   |
             Videodec2 <----------+
                |
                v
       SDR NV12 AGC presenter -> VideoOut
```

VP9 joins this design through a bounded incremental WebM demuxer that emits
Profile 0 coded blocks directly to the native packet/Videodec2 boundary.

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
| iptv-org | `/download0/prosperotv-catalog.sqlite3` |
| Custom M3U/M3U8 | `/download0/prosperotv-custom-catalog.sqlite3` |

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
HLS master playlists are reduced to variants whose declared codec, profile,
level, dimensions, and bandwidth fit the configured native limits. Valid
nonstandard dimensions are mapped to the smallest compatible decoder capacity
class rather than being rejected by an exact-resolution allowlist.

The media path currently accepts:

- direct MPEG-TS streams;
- HLS media playlists whose segments contain MPEG-TS;
- direct video-only WebM streams carrying VP9 Profile 0;
- H.264 stream type `0x1b` or HEVC stream type `0x24`;
- optional AAC ADTS audio.

The MPEG-TS layer discovers PAT/PMT state, assembles bounded PES payloads,
extracts codec configuration from SPS data, emits complete Annex-B access
units, and preserves presentation timestamps for native pacing.

It does not currently accept fragmented MP4, DASH, WebM audio, or general
Matroska features. MPEG-TS remains limited to H.264/HEVC plus optional AAC;
VP9 Profile 0 uses the direct WebM path.

## Native video backend

The decoder owns distinct input and frame pools and validates every requested
codec/profile/geometry combination against capacity classes before opening
Videodec2. H.264, HEVC, and VP9 Profile 0 use a one-frame caller-owned decoder
pipeline, matching the hardware investigation results.

VP9 coded packets are split using the trailing superframe index. Every coded
frame is submitted in packet order, including hidden frames. The fixed
uncompressed-header prefix is parsed so hidden frames are decoded without
being presented, while `show_frame` and `show_existing_frame` events remain in
submission order.

### Configured hardware modes

| Codec | Profile | Coded / visible geometry | Maximum configured level | Output pitch |
| --- | --- | --- | --- | --- |
| H.264 | Baseline, Main, High | Up to 1280×720 | 41 | Decoder-reported |
| H.264 | High | Up to 1920×1088 | 51 | Decoder-reported |
| H.264 | High | Up to 2560×1440 | 51 | Decoder-reported |
| H.264 | High | Up to 3840×2176 | 52 | Decoder-reported |
| HEVC | Main, 8-bit 4:2:0 | Up to 1280×720 | 123 | Decoder-reported |
| HEVC | Main, 8-bit 4:2:0 | Up to 1920×1088 | 123 | Decoder-reported |
| HEVC | Main, 8-bit 4:2:0 | Up to 2560×1440 | 150 | Decoder-reported |
| HEVC | Main, 8-bit 4:2:0 | Up to 3840×2176 | 153 | Decoder-reported |
| VP9 | Profile 0, 8-bit 4:2:0 | Up to 1920×1080 | 41 | Decoder-reported |
| VP9 | Profile 0, 8-bit 4:2:0 | Up to 2560×1440 | 50 | Decoder-reported |
| VP9 | Profile 0, 8-bit 4:2:0 | Up to 3840×2160 | 51 | Decoder-reported |

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
registration, full-frame viewport/scissor state, and allocation size are
recreated when the output class changes.

## Cleanup and diagnostics

Native decoder, audio, AGC, VideoOut, network, UI, and direct-memory resources
have explicit stop/drain/close paths. Playback writes a bounded diagnostic
receipt to `/download0/iptv-last-receipt.txt`, including codec, stream format,
decoded/presented counts, pipeline drain state, timing, and zero-copy pointer
observations. This receipt is intended to complement crash and kernel logs in
the project's investigation loop; it is not a substitute for hardware
acceptance testing.
