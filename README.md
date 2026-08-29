# psiptv

`psiptv` is a native IPTV client for PlayStation 5. Its PS5 title ID is
`PPSA88000`, its display name is `psiptv`, and its current content version is
`01.000.001`.

The project is derived from
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate)
and follows the proven application structure and operating patterns of
[PSRadio](https://github.com/blackbearreloaded/ps5-radio): a native SDL/RmlUi
shell, controller-first navigation, background network work, persistent state
under `/download0`, and explicit cleanup of platform resources.

The built-in channel source is the community-maintained
[iptv-org/iptv](https://github.com/iptv-org/iptv) catalog, fetched from
`https://iptv-org.github.io/iptv/index.m3u`. psiptv is an independent client;
it does not operate or control the streams listed by iptv-org.

## Current capabilities

- Loads the last good catalog immediately from
  `/download0/psiptv-catalog.sqlite3` and refreshes iptv-org in a background
  thread.
- Parses extended M3U metadata, alternate URLs and groups, countries,
  languages, and per-channel user-agent/referrer directives.
- Supports a separately cached custom HTTP(S) M3U/M3U8 source at
  `/download0/psiptv-custom-catalog.sqlite3`.
- Provides native browsing, search, category filters, favorites, recent
  channels, and source management.
- Plays HLS or direct MPEG-TS streams carrying H.264 or 8-bit HEVC video, with
  optional AAC ADTS audio, plus direct WebM streams carrying VP9 Profile 0.
- Uses the native Videodec2 and AGC path for hardware decode and presentation.
- Contains a native VP9 Profile 0 backend, including superframe splitting,
  hidden-frame ordering, and 1080p/1440p/2160p decode modes.

## Video modes and output

| Codec | Implemented hardware modes | Current end-to-end channel path |
| --- | --- | --- |
| H.264/AVC | 720p, 1080p, 1440p, 2160p; Baseline/Main/High within each mode's configured level limit | Yes, through MPEG-TS/HLS |
| HEVC | Main profile, 8-bit 4:2:0 at 720p, 1080p, 1440p, and 2160p | Yes, through MPEG-TS/HLS |
| VP9 | Profile 0, 8-bit 4:2:0 at 1080p, 1440p, and 2160p | Yes, through direct WebM |

Output geometry is selected from the decoded source size:

| Decoded source | VideoOut surface | Presentation |
| --- | --- | --- |
| Up to 1920×1080 | 1920×1080 | Native AGC presentation |
| 2560×1440 | 3840×2160 | Native decode, bilinear GPU scaling to 4K output |
| 3840×2160 | 3840×2160 | Native 4K decode and output |

“Native” here describes the PS5 hardware decode/presentation pipeline. It does
not mean that VideoOut is configured to a separate 2560×1440 mode; 1440p video
is presented on the 4K surface.

## Important limitations

- VP9 delivery currently supports direct, video-only WebM Profile 0 streams.
  DASH, fragmented MP4, WebM audio, and more general Matroska features are not
  implemented. MPEG-TS video remains limited to H.264 and HEVC.
- VP9 Profile 2 is not supported. Its 10-bit low-aligned output requires a
  dedicated presentation shader/path; the current presenter is SDR NV12.
- The catalog is persisted in SQLite but loaded into an in-memory model for UI
  navigation rather than queried page by page.
- Merged alternate stream URLs are tried automatically when a primary URL
  fails or ends without a successful playback result.
- Startup, H.264/HEVC playback, direct WebM VP9 playback, alternate-URL
  fallback, timed cancellation, and repeated decoder teardown have passed
  bounded PS5 acceptance. The remaining controller/cache behavior and soak
  gates still prevent a release-ready claim.

See [Architecture](docs/ARCHITECTURE.md) for component and codec details and
[Status](docs/STATUS.md) for the implementation boundary and remaining work.

## Build

Use Linux or WSL with the prerequisites documented by the native boilerplate:

```bash
make doctor
make test
make app
```

The complete folder title is emitted at `dist/PPSA88000/`. Optional package
formats are available through `make ffpkg` and `make ffpfsc`. Deploy the whole
title, not `eboot.bin` alone. PS5 testing and deployment must follow the shared
console lock and investigation protocol.

## Persistent files

| Path | Purpose |
| --- | --- |
| `/download0/psiptv-catalog.sqlite3` | Last good built-in iptv-org catalog |
| `/download0/psiptv-custom-catalog.sqlite3` | Last good custom catalog |
| `/download0/iptv-favorites-v1.bin` | Favorite channel IDs |
| `/download0/iptv-history-v1.bin` | Recent channel IDs |
| `/download0/iptv-custom-source-v1.txt` | Custom playlist URL |
| `/download0/iptv-active-source-v1.txt` | Active source selection |
| `/download0/iptv-last-receipt.txt` | Most recent playback diagnostic receipt |

## License

The repository is licensed under GPL-3.0-or-later. Third-party components and
data retain their own licenses and terms; see [NOTICE.md](NOTICE.md).
