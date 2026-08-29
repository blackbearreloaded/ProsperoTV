# psiptv project plan

Updated: 2026-08-29 America/New_York
Status: active

## Overview

- Problem: provide a fast, controller-first native PS5 IPTV client that starts
  reliably, caches its catalog locally, and uses the console's hardware video
  decoder and native presentation path.
- Intended users: PS5 homebrew users with access to lawful public or personal
  IPTV playlists.
- Scope and functionality: iptv-org and custom M3U catalogs, SQLite caching,
  browsing/search/favorites/history, HLS and direct MPEG-TS playback, AAC audio,
  native H.264/HEVC playback, and direct WebM VP9 Profile 0 playback.
- Deliverables: source, tests, signed title folder, optional FFPFSC image,
  deployment tooling, hardware evidence, and an accurate status record.
- Non-goals: DRM, encrypted streams, provider credentials, console settings
  changes, proprietary assets in Git, or unsupported firmware claims.

## Requirements

### Functional

- `R1`: start as `psiptv` under title `PPSA88000` and render the native UI.
- `R2`: load the last good catalog from `/download0` before network refresh.
- `R3`: fetch and parse the official iptv-org `index.m3u` off the UI thread.
- `R4`: persist built-in and custom catalogs in separate bounded SQLite files
  using staged replacement, integrity checks, and last-good recovery.
- `R5`: provide controller navigation, search, groups, favorites, history,
  source selection, refresh state, and actionable errors.
- `R6`: play direct MPEG-TS and HLS carrying H.264 or HEVC, with optional AAC,
  and try catalog alternate URLs after a pre-playback failure.
- `R7`: hardware-decode H.264 and HEVC at arbitrary valid dimensions within
  hardware-proven capacity classes through 720p, 1080p, 1440p, and 2160p.
- `R8`: present up to 1080p on a native 1080p target, scale 1440p bilinearly to
  a native 4K target, and present 2160p 1:1 on the native 4K target.
- `R9`: support bounded direct WebM delivery and VP9 Profile 0 backend
  operation at 1080p, 1440p, and 2160p, including superframe splitting,
  hidden-frame ordering, and caller-owned buffers.
- `R10`: persist bounded playback telemetry sufficient to classify network,
  demux, decoder, presenter, stop, and cleanup outcomes.

### Non-functional

- `N1`: all untrusted network, playlist, database, and packet sizes are bounded
  and validated before allocation or indexing.
- `N2`: catalog/network work does not block frame rendering; network producer,
  decoder, and completed-flip presentation remain separable for VP9 delivery.
- `N3`: shutdown is deterministic and releases network, decoder, audio,
  presenter, SDL, and worker resources in ownership order.
- `N4`: host tests, formatting, lint, signed-container inspection, exact
  artifact hashes, and the boilerplate PRX/title-layout invariant pass before
  every hardware candidate.
- `N5`: no proprietary shader, SDK, credential, fixture, or console artifact is
  committed.

## Constraints and limitations

- The target is the shared firmware-6.02 PS5 at `192.168.4.30`; console contact
  requires FTP 2121, klog 3232, elfldr/shsrv 9021, and exclusive ownership of
  the environment-declared shared lock for the entire bounded cycle.
- Never open or change PS5 Settings and never initiate or approve an update.
- iptv-org provides playlist metadata, not stream availability or legal rights.
- The UI currently materializes the bounded selected catalog in memory after
  loading it from SQLite. Database-backed paging is measurement-driven follow-up.
- VP9 Profile 0 is accepted through a bounded video-only WebM path. DASH,
  fragmented MP4, WebM audio, and general Matroska features remain outside the
  current delivery contract.
- VP9 Profile 2 is hardware-proven by the research project, including its
  low-aligned 10-bit surface. It remains deferred here until psiptv has a
  redistributable 10-bit AGC shader/path and VP9 transport.
- Hardware risk is controlled with one frozen candidate, title-specific logs,
  bounded observation, explicit close, service health checks, and no speculative
  retry after a real execution failure.

## Project acceptance criteria

| ID | Observable criterion | Required evidence |
| --- | --- | --- |
| `A1` | ShadowMount registers `PPSA88000` and the title reaches a stable rendered UI without a crash. | Frozen hashes, lifecycle/klog slice, screenshot, clean close |
| `A2` | A clean data directory refreshes iptv-org into SQLite; relaunch loads the cache before refresh. | UI evidence plus `/download0` files and receipt |
| `A3` | Browse, search, favorite, recent, source, and refresh controls work from the controller. | Bounded functional run and persisted state |
| `A4` | H.264 and HEVC play at 1080p, 1440p, and 2160p with expected output geometry and clean stop. | Per-mode receipt, screenshot, klog, clean teardown |
| `A5` | Alternate URL, cancellation, repeated channel changes, and network failure remain responsive and leak-free. | Scenario receipts and post-run service health |
| `A6` | VP9 Profile 0 is reachable through a bounded transport and presents 1080p, 1440p, and 2160p correctly. | Controlled fixtures, coded/hidden/display counts, screenshots |
| `A7` | VP9 Profile 2 presents its low-aligned 10-bit surface only through a redistributable validated path. | 1080p/4K controlled HDR evidence; no private asset in Git |

## Goal map

| Goal | Outcome | Dependencies | Status |
| --- | --- | --- | --- |
| `G1` | Stable native shell, catalog cache, and launcher lifecycle | None | complete |
| `G2` | Complete H.264/HEVC IPTV path and resolution matrix | `G1` | complete |
| `G3` | Resilience, channel switching, and repeated-run stability | `G2` | complete (bounded) |
| `G4` | End-to-end VP9 Profile 0 delivery | `G2` | complete |
| `G5` | Redistributable VP9 Profile 2/10-bit presentation | `G4` | deferred |

## G1: Native shell and catalog

- Status: complete
- Objective: prove launcher discovery, startup UI, clean first-run catalog
  refresh, cache-first relaunch, controller basics, and deterministic teardown.
- Requirements advanced: `R1`-`R5`, `R10`, `N1`, `N3`, `N4`.
- Dependencies: owner-started shared-console services and lock availability.
- Deliverables: frozen signed title, SQLite cache, startup/refresh receipts, one
  investigation-loop result, and one milestone line.
- Limitations and risks: prior prototypes crashed or were not discovered;
  evidence must distinguish registration, loader, app, and UI stages.
- Acceptance criteria: `A1`, `A2`, and the startup subset of `A3`.
- Required evidence: exact hashes; title-specific ShadowMount and klog slice;
  rendered screenshot; clean title close; service health.
- Candidate commit: `5eda2eb` (all `PT_LOAD` addresses are page-congruent, the
  executable preserves the boilerplate's separate packaged-libc facade and
  Sony libc implementation dependencies, and app code owns a 32-byte-aligned
  page-backed allocator family without modifying `libc.prx`).
- Validation-record commit: pending.

## G2: Native H.264/HEVC playback

- Status: complete
- Objective: prove the IPTV stream adapter, native decoder, audio, pacing, and
  1080p/1440p/2160p output geometry with controlled legal-safe fixtures.
- Requirements advanced: `R6`-`R8`, `R10`, `N2`-`N4`.
- Dependencies: `G1` and locally hosted MPEG-TS/HLS fixtures.
- Deliverables: per-codec/per-resolution receipts and screenshots.
- Limitations and risks: external catalog streams are not deterministic test
  controls and cannot substitute for fixture evidence.
- Acceptance criteria: `A4`.
- Required evidence: exact candidate hash, decoded/presented counts, output
  class, screenshot, title-specific logs, and clean teardown.
- Candidate commit: `0e92b6b`.
- Candidate artifact: `eboot.bin` SHA-256
  `4055f6f09c41a2cd071e0df20728105309ee8b746b7eb54f2c29622c8960df22`.
- Validation-record commit: pending.

## G3: Playback resilience

- Status: complete for the bounded acceptance case
- Objective: validate alternate URLs, cancellation, repeated channel changes,
  network interruption, cached relaunch, and resource reuse.
- Requirements advanced: `R5`, `R6`, `R10`, `N2`, `N3`.
- Dependencies: `G2`.
- Deliverables: bounded failure fixtures, receipts, and repeated-run evidence.
- Limitations and risks: every scenario must have a deterministic stop condition.
- Acceptance criteria: `A5`.
- Required evidence: scenario receipts, klog slices, and post-run health checks.
- Candidate commit: `51762cb`.
- Candidate artifact: `eboot.bin` SHA-256
  `0b1aa33e134f8862c10084494b906dc48619a3be89c2ff49441877785bc08957`.
- Accepted case: disposable title `PPSA88015` decoded and presented 300 H.264
  frames before the primary HLS fixture failed, selected the second URL, then
  completed it. A timed HEVC live stream cancellation presented 149 frames and
  four subsequent H.264/HEVC sessions presented 300/330/300/330 frames. Every
  attempt reported zero stream, player, and native cleanup error; the app
  remained alive and returned to the launcher.
- Evidence: `results/G3/PPSA88015-20260829-151056-result.json` and
  `results/G3/PPSA88015-20260829-151056-resilience-receipts.txt`.
- Validation-record commit: protocol `83367cd`.
- Residual release soak: repeat against a deliberately stalled socket and for
  a longer channel-change duration; this does not invalidate the bounded A5
  pass.

## G4: VP9 Profile 0 delivery

- Status: complete for direct WebM Profile 0
- Objective: connect a bounded WebM producer to the existing Profile 0 backend
  without blocking network receive on decode or completed flip.
- Requirements advanced: `R9`, `R10`, `N1`-`N4`.
- Dependencies: `G2`; controlled Profile 0 fixtures.
- Deliverables: bounded incremental WebM demux, direct player adapter,
  packet-order regression tests, and end-to-end playback.
- Limitations and risks: preserve per-coded-frame boundaries, split compound
  superframes, submit hidden frames, and retain show-frame/show-existing order.
- Acceptance criteria: `A6`.
- Required evidence: 1080p/1440p/2160p counts, screenshot, logs, and teardown.
- Candidate commit: `611765f` (WebM parser `39f7da5`, player integration
  `d285656`, and VP9 specification-bit-order correction `611765f`).
- Candidate artifact: `eboot.bin` SHA-256
  `ada059836f626f7d68697eff5af51a89770663d251ae0809d2f3452acdaa3264`.
- Accepted case: disposable title `PPSA88017` decoded and presented 30/30
  Profile 0 frames at 1920x1080, 2560x1440, and 3840x2160. All three used the
  native frame pool with matching zero-copy pointers and reported zero native,
  stream, and player cleanup errors. The first launch was inconclusive before
  eboot because of stale ShadowMount registration; the one permitted
  identical-byte retry entered eboot and returned to the launcher cleanly.
- Evidence: `results/G4/PPSA88017-20260829-161418-result.json` and
  `results/G4/PPSA88017-20260829-161418-g4-vp9-receipts.txt`.
- Validation harness: protocol `ec87e00`.

## G5: VP9 Profile 2 presentation

- Status: deferred
- Objective: support the hardware-proven low-aligned two-plane 10-bit output
  through a redistributable AGC conversion/presentation path.
- Requirements advanced: future extension of `R9`, `N4`, and `N5`.
- Dependencies: `G4` and a redistributable shader/build path.
- Deliverables: 10-bit texture contract, SDR/HDR policy, and hardware evidence.
- Limitations and risks: the existing research shader is private and must not be
  copied into Git; the surface is not conventional MSB-aligned P010.
- Acceptance criteria: `A7`.
- Required evidence: controlled 1080p and 4K Profile 2 decode/presentation,
  validated bands/color, exact hashes, and publication-boundary scan.
- Candidate commit: pending.
- Validation-record commit: pending.
