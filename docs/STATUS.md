# Project status

Status date: 2026-08-29

psiptv has a coherent native application, catalog cache, MPEG-TS/HLS playback
path, and hardware decoder/presenter backend. It is an integration build, not
yet a release: the current source still marks stream hardware acceptance as
unvalidated, and the latest combined application requires a complete PS5
investigation-loop run.

## Implemented

| Area | Current state |
| --- | --- |
| Identity | Configured as `psiptv`, title `PPSA88000`, concept `88000`, content version `01.000.001` |
| Native foundation | Boilerplate build/runtime with byte-identical generated `libc.prx`, PSRadio-style SDL/RmlUi shell, controller input, background work, `/download0` state, and explicit cleanup |
| Public catalog | Built-in iptv-org `index.m3u` source plus a user-entered custom HTTP(S) M3U/M3U8 source |
| Catalog parser | Extended M3U metadata, deduplication, alternate URLs/groups, country/language fields, and channel HTTP headers |
| Local cache | Separate SQLite databases for built-in and custom sources, staged replacement, quick integrity check, and last-good fallback |
| Browser UI | Channel list, details, search, broad groups, favorites, recents, source selection, and background refresh state |
| Transport | HTTP(S), redirects, direct MPEG-TS, HLS master/media playlists, MPEG-TS PAT/PMT/PES, and optional AAC ADTS |
| H.264 | Native hardware modes for 720p, 1080p, 1440p, and 2160p |
| HEVC | Native Main 8-bit hardware modes for 720p, 1080p, 1440p, and 2160p |
| VP9 Profile 0 backend | Native 1080p, 1440p, and 2160p modes; superframe splitting; hidden/display frame ordering; depth-three decode pipeline |
| Presentation | SDR NV12 AGC presenter with 1080p and 4K VideoOut classes; bilinear 1440p-to-4K scaling |
| Diagnostics | Persistent playback receipt and native decode/present telemetry |
| Build output | Native title folder plus optional `.ffpkg` and `.ffpfsc` packaging paths |

## Incomplete or not yet proven

### VP9 end-to-end delivery

The VP9 Profile 0 packetizer and Videodec2 backend exist, but normal channel
playback cannot select them. The current stream contract recognizes only H.264
and HEVC from MPEG-TS, and the player adapter maps only those two codecs.

Completing VP9 delivery requires a bounded container/manifest path—most likely
WebM/Matroska and/or DASH—plus a stream contract that passes VP9 coded packets,
profile, level, geometry, and timestamps to the existing backend. This should
be tested with superframes, hidden frames, and `show_existing_frame` events.

### VP9 Profile 2 presentation

Profile 2 is not accepted by the current backend. Its low-aligned 10-bit 4:2:0
output is incompatible with the current 8-bit NV12 shader. Completion requires
a dedicated 10-bit surface contract and AGC conversion/presentation path,
followed by 1080p and 4K hardware validation. It must not be enabled by simply
adding Profile 2 to the mode table.

### Hardware acceptance

The mode tables and presenter geometry reflect hardware investigation results,
but this integrated `PPSA88000` application has not completed its final
console acceptance pass. The loader now reaches `_start`. Hardware confirmed
that both required libc dependencies load, but system `calloc` still returns
null under the clean-room boilerplate heap contract. Candidate `59fe40c`
therefore provides a bounded app-local `calloc` using the working allocator
while preserving the boilerplate-generated `libc.prx` byte-for-byte. Startup
must be rerun, followed by catalog refresh, launcher return, repeated channel
changes, H.264/HEVC at each geometry, audio/video pacing, cancellation, and
cleanup on the target console.

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
  failure. Per-URL health scoring and cooldowns are not yet implemented.
- Fragmented MP4 HLS, DASH, WebM/Matroska, DRM, and encrypted media are not
  supported.
- The HLS selector accepts only the explicit 720p, 1080p, 1440p, and 2160p
  geometries; unusual dimensions are rejected rather than dynamically mapped.
- Profile, level, bit depth, chroma, and geometry must match a configured
  native mode exactly enough for the backend to open.
- Stream availability and legality are properties of each third-party listing,
  not guarantees made by psiptv or iptv-org.

## Next completion gates

1. Run host tests and a clean `make app`, recording the exact artifact digest.
2. Acquire the shared console lock and execute the investigation-loop startup
   and launcher-discovery checks for `PPSA88000`.
3. Validate cached startup and background refresh from a clean `/download0`,
   then validate relaunch using only the saved SQLite cache.
4. Exercise H.264 and HEVC at 1080p, 1440p, and 2160p, confirming decoded and
   presented counts, output-class switching, pacing, audio, stop, and cleanup.
5. Add a bounded VP9 delivery/container adapter and validate Profile 0 at all
   three configured geometries.
6. Add and validate the 10-bit AGC presentation path before exposing VP9
   Profile 2.
7. Run long-duration alternate-URL, channel-change, and cancellation tests.

Release readiness requires all applicable gates to pass on hardware without a
startup crash, resource leak across repeated playback, or stale ShadowMount
artifact ambiguity.
