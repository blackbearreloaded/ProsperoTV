# Project status

Status date: 2026-08-29

psiptv has a hardware-proven native shell and local iptv-org catalog cache plus
an integrated MPEG-TS/HLS playback and hardware decoder/presenter path. H.264
and HEVC are hardware-proven from sub-720p through native 4K. Bounded fallback,
cancellation, repeated decoder teardown, and direct WebM VP9 Profile 0 at
1080p, 1440p, and 2160p are also hardware-accepted. It is not yet a release:
remaining UI/cache behavior and long-duration soak remain.

## Implemented

| Area | Current state |
| --- | --- |
| Identity | Configured as `psiptv`, title `PPSA88000`, concept `88000`, content version `01.000.001` |
| Native foundation | `ps5-native-app-boilerplate` Makefile, native linker/module writer, SELF signer, title-folder assembler, and deterministic generated `libc.prx`; PSRadio-style SDL/RmlUi shell, controller input, background work, `/download0` state, and explicit cleanup |
| Public catalog | Built-in iptv-org `index.m3u` source plus a user-entered custom HTTP(S) M3U/M3U8 source |
| Catalog parser | Extended M3U metadata, deduplication, alternate URLs/groups, country/language fields, and channel HTTP headers |
| Local cache | Separate SQLite databases for built-in and custom sources, staged replacement, quick integrity check, and last-good fallback |
| Browser UI | Channel list, details, search, broad groups, favorites, recents, source selection, and background refresh state |
| Transport | HTTP(S), redirects, direct MPEG-TS, HLS master/media playlists, MPEG-TS PAT/PMT/PES, optional AAC ADTS, and bounded direct WebM video |
| H.264 | Native hardware capacity classes through 720p, 1080p, 1440p, and 2160p; arbitrary valid dimensions within a class are accepted |
| HEVC | Native Main 8-bit capacity classes through 720p, 1080p, 1440p, and 2160p; arbitrary valid dimensions within a class are accepted |
| VP9 Profile 0 | Bounded incremental WebM demux; native 1080p, 1440p, and 2160p capacity classes; superframe splitting; hidden/display frame ordering; one-frame caller-owned pipeline |
| Presentation | SDR NV12 AGC presenter with 1080p and 4K VideoOut classes; bilinear 1440p-to-4K scaling |
| Diagnostics | Persistent playback receipt and native decode/present telemetry |
| Build output | Native title folder plus optional `.ffpkg` and `.ffpfsc` packaging paths |

## Incomplete or not yet proven

### VP9 delivery boundary

Direct video-only WebM Profile 0 playback is complete and hardware-accepted.
DASH, fragmented MP4, WebM audio, and general Matroska features are not part of
the current bounded demux contract.

### VP9 Profile 2 presentation

Profile 2 is not accepted by the current backend. Its low-aligned 10-bit 4:2:0
output is incompatible with the current 8-bit NV12 shader. Completion requires
a dedicated 10-bit surface contract and AGC conversion/presentation path,
followed by 1080p and 4K hardware validation. It must not be enabled by simply
adding Profile 2 to the mode table.

### Hardware acceptance

Candidate `5eda2eb` passed the G1 investigation-loop startup case. The exact
FFPFSC entered `eboot.bin`, rendered the native browser with 12,863 channels
from the cached iptv-org catalog, remained alive through observation, and
returned to the launcher on close. The current G2 candidate then passed
controlled hardware playback for H.264 at 960x540, 1920x1080, 2560x1440, and
3840x2160, and HEVC at 1280x720, 1920x1080, 2560x1440, and 3840x2160. Every
accepted fixture decoded and presented all video access units from the native
frame pool with matching zero-copy pointers and decoded AAC audio.

Candidate `51762cb` then passed the strict G3 cycle as disposable title
`PPSA88015`. A controlled HLS primary failed after decoding/presenting 300
H.264 frames; the normal catalog fallback helper selected and completed the
second URL. Timed HEVC cancellation presented 149 frames, followed by four
successful H.264/HEVC sessions presenting 300/330/300/330 frames. All six
tests reported zero stream, player, and native cleanup error, no crash was
observed, and the title returned to the launcher. The exact accepted
`eboot.bin` SHA-256 is
`0b1aa33e134f8862c10084494b906dc48619a3be89c2ff49441877785bc08957`.

Candidate `611765f` then passed strict G4 acceptance as disposable title
`PPSA88017`. Direct WebM Profile 0 fixtures at 1920x1080, 2560x1440, and
3840x2160 each decoded and presented 30/30 frames from the native frame pool
with matching zero-copy pointers. Every native, stream, and player cleanup
result was zero, and the title returned to the launcher. The exact accepted
`eboot.bin` SHA-256 is
`ada059836f626f7d68697eff5af51a89770663d251ae0809d2f3452acdaa3264`.

The build remains structurally based on `ps5-native-app-boilerplate`: it uses
the boilerplate's native linker/module writer and SELF/container validation,
assembles `eboot.bin`, `sce_sys`, and `sce_module`, and copies the verified
generated runtime to `sce_module/libc.prx`. The source and packaged runtime are
both 1,284,674 bytes with SHA-256
`e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036`.
Application-specific import facades and the 32-byte-aligned executable
allocator extend that foundation without replacing its packaging structure.

Testing must use the shared PS5 lock and the repository's investigation-loop
protocol. A crash or black screen requires evidence collection before another
change; repeated speculative deploys are not an acceptance method.

### Catalog scalability

SQLite provides a durable local cache, but the entire selected catalog is
currently materialized into memory and filtered by the UI. This is functional
for the bounded catalog size, but true database-backed paging/search remains a
possible optimization if measurements show startup latency or memory pressure.

### Playback resilience

- Alternate URLs are attempted in catalog order after a primary stream
  failure. Mid-playback HLS failure and selection of the second candidate have
  passed the bounded hardware case. Per-URL health scoring and cooldowns are
  not implemented.
- Timed cancellation and four consecutive H.264/HEVC session teardowns passed
  with explicit zero cleanup results. A longer soak and a deliberately stalled
  socket remain release-hardening cases.
- Fragmented MP4 HLS, DASH, WebM audio, general Matroska, DRM, and encrypted
  media are not supported.
- HLS and the native backend accept arbitrary valid dimensions that fit a
  decoder capacity class; profile, level, bit depth, and chroma are still
  bounded to hardware-proven SDR modes.
- Stream availability and legality are properties of each third-party listing,
  not guarantees made by psiptv or iptv-org.

## Next completion gates

1. Exercise controller browsing, clean `/download0` refresh, and cache-only
   relaunch as explicit functional cases.
2. Add and validate the 10-bit AGC presentation path before exposing VP9
   Profile 2.
3. Run long-duration alternate-URL, channel-change, cancellation, and stalled
   socket tests.

Release readiness requires all applicable gates to pass on hardware without a
startup crash, resource leak across repeated playback, or stale ShadowMount
artifact ambiguity.
