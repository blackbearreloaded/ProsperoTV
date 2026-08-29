# psiptv handoff

Updated: 2026-08-29 America/New_York

## Current accepted candidate

- Goal: `G4` end-to-end VP9 Profile 0 delivery, complete for direct WebM.
- Source commit: `611765f` (strict G4 hardware acceptance passed).
- Title: `PPSA88000`, version `01.000.001`, firmware target 6.02.
- App folder: `dist/PPSA88000/`.
- FFPFSC: `dist/PPSA88000.ffpfsc`.
- Host gates: 39 unit tests, 6 tooling tests, lint, native build,
  signed-container integrity, MkPFS creation, and MkPFS verification passed.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `eboot.bin` | 5,432,066 | `ada059836f626f7d68697eff5af51a89770663d251ae0809d2f3452acdaa3264` |
| `sce_module/libc.prx` | 1,284,674 | `e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036` |
| `sce_sys/param.json` | 883 | `2acfea4e4f7957e254f07d989bb5b97b92e1d4064467c24109befaa73990d856` |
| `sce_sys/icon0.png` | 355,883 | `282ede549c8118855fc1e1a808703f13ccb1809fdaaaa6f287ac8f2977f053cf` |
| `sce_sys/pic0.dds` | 8,294,548 | `56a982da83853ffce3cf62f6cc169016a0ad4479a3e6082a2b6390eebdbd264b` |
| `PPSA88000.ffpfsc` | 37,486,592 | `0dd94e7fc24e1056678f22544f32549e3762616c3bac8c407f0af25d39e4f7e5` |

## Boilerplate invariant

- `Makefile` and `tools/build.sh` retain the `ps5-native-app-boilerplate`
  compile, native link/module-write, SELF-sign, inspection, app-folder, and
  MkPFS verification flow.
- `runtime/libc.prx` is hash-verified before packaging and copied to
  `dist/PPSA88000/sce_module/libc.prx`; both copies are 1,284,674 bytes and
  SHA-256 `e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036`.
- A host integration test now guards the runtime digest and required title
  layout/signing steps against accidental removal.

## Next bounded case

- Acceptance: controller browse/search/filter, favorite/history persistence,
  source selection, clean catalog refresh, and cache-first relaunch all work in
  one bounded functional cycle.
- Control: the accepted production title with a known cached iptv-org catalog
  and one clean `/download0` refresh/relaunch sequence.
- Stop condition: any Settings, Store, sign-in, update, ambiguous screen,
  execution crash, kernel-health concern, hash mismatch, or failed cleanup.
- Use the maintained `ps5-homebrew-dev-protocol` IPTV wrapper. It owns the
  environment lock for deployment, observation, evidence, close, and cleanup.

## Milestones

- 2026-08-29 | G1 | c0a3d7f | host | partial-pass: frozen and packaged | docs/HANDOFF.md | run locked startup case
- 2026-08-29 | G1 | c0a3d7f | fw6.02 | failed: eboot exec LOAD 0x404ad0 rejected before _start | results/G1/PPSA88000-20260829-093633-result.json | shrink exec LOAD
- 2026-08-29 | G1 | 799eafa | host | partial-pass: size-only check missed non-congruent RELRO LOAD | docs/HANDOFF.md | validate every LOAD
- 2026-08-29 | G1 | 799eafa | fw6.02 | failed: segment 2 offset 0x4dfcc0 not page-congruent with VA 0x4c8000 | results/G1/PPSA88000-20260829-100655-result.json | fix RELRO file anchor
- 2026-08-29 | G1 | 3180560 | host | pass: tests, lint, signed containers, MkPFS, and all LOAD congruence checks | docs/HANDOFF.md | remount exact image and run startup case
- 2026-08-29 | G1 | 3180560 | fw6.02 | failed: EXEC succeeded, then bitmap font map aborted when `calloc` resolved through the packaged facade | results/G1/PPSA88000-20260829-101904-result.json | preserve PSRadio dual-libc dependency graph
- 2026-08-29 | G1 | 4caffd1 | host | pass: 25 unit tests, 5 tooling tests, lint, signed containers, dual-libc build gate, MkPFS creation, and verification | docs/HANDOFF.md | remount exact image and run startup case
- 2026-08-29 | G1 | 4caffd1 | fw6.02 | failed: both libc dependencies loaded, but system `calloc` returned null under the clean-room boilerplate heap contract | results/G1/PPSA88000-20260829-103844-result.json | provide bounded app-local `calloc`
- 2026-08-29 | G1 | 59fe40c | host | pass: 25 unit tests, 5 tooling tests, lint, local-`calloc` binding gate, unchanged boilerplate `libc.prx`, signed containers, MkPFS creation, and verification | docs/HANDOFF.md | remount exact image and run startup case
- 2026-08-29 | G1 | 59fe40c | fw6.02 | failed: app-local `calloc` reached the same first font-map allocation failure because its underlying system `malloc` also returned null | results/G1/PPSA88000-20260829-105947-result.json | own the complete executable allocator family
- 2026-08-29 | G1 | 8dc7f8b | host | pass: 25 unit tests, 5 tooling tests, lint, no unresolved allocator-family imports, unchanged boilerplate `libc.prx`, signed containers, static inspection, MkPFS creation, and verification | docs/HANDOFF.md | remount exact image and run startup case
- 2026-08-29 | G1 | 8dc7f8b | fw6.02 | inconclusive twice: first launch overlapped PPSA77003; second was rejected before EXEC by stale inner LVD read error 5 | results/G1/PPSA88000-20260829-112516-result.json | require explicit two-layer teardown
- 2026-08-29 | G1 | 8dc7f8b | fw6.02 | failed: clean remount reached EXEC, then ordinary `operator new` returned 16-byte alignment to a 256-bit RmlUi initializer | results/G1/PPSA88000-20260829-113041-result.json | align allocator family to 32 bytes
- 2026-08-29 | G1 | 5eda2eb | host | pass: 25 unit tests, 5 tooling tests, lint, deterministic boilerplate `libc.prx`, signed containers, static inspection, MkPFS creation, and verification | docs/HANDOFF.md | clean two-layer remount and startup case
- 2026-08-29 | G1 | 5eda2eb | fw6.02 | pass: exact image entered eboot, rendered native UI with 12,863 cached iptv-org channels, remained alive, and returned to launcher on close | results/G1/PPSA88000-20260829-114721-result.json | begin controlled codec matrix
- 2026-08-29 | G2 | 4aa583a eboot | fw6.02 | pass: H.264 High and HEVC Main 1080p60, 1,800/1,800 decoded/presented frames each, AAC decoded, zero-copy native frame-pool output | results/G2/PPSA88002-20260829-115934-codec-receipts.txt | broaden geometry classes
- 2026-08-29 | G2 | 0e92b6b | host | pass: 28 unit tests, 6 tooling tests, lint, boilerplate runtime hash/layout gate, native build, SELF inspection, MkPFS creation and verification | docs/HANDOFF.md | run capacity-class matrix
- 2026-08-29 | G2 | 4055f6f eboot | fw6.02 | pass: H.264 960x540 and HEVC 1280x720 decoded/presented 60/60 with AAC and zero-copy native output | results/G2/PPSA88004-20260829-122922-codec-receipts.txt | test upper classes
- 2026-08-29 | G2 | 4055f6f eboot | fw6.02 | pass: H.264 3840x2160 and HEVC 2560x1440 decoded/presented 30/30 with AAC and zero-copy native output | results/G2/PPSA88005-20260829-123302-codec-receipts.txt | test complementary upper classes
- 2026-08-29 | G2 | 4055f6f eboot | fw6.02 | pass: H.264 2560x1440 and HEVC 3840x2160 decoded/presented 30/30 with AAC and zero-copy native output | results/G2/PPSA88006-20260829-123637-codec-receipts.txt | begin resilience work
- 2026-08-29 | G2 | 4055f6f eboot | fw6.02 | inconclusive before app execution: ShadowMount lost and reattached the outer PFSC device without rebuilding the inner exFAT layer | results/G2/PPSA88007-20260829-124918-result.json | retry unchanged artifact under a fresh disposable title
- 2026-08-29 | G2 | 4055f6f eboot | fw6.02 | fixture rejected: both 1080p codecs decoded/presented 90/90, but the first mux emitted no parsable AAC frames | results/G2/PPSA88008-20260829-125240-codec-receipts.txt | correct only the controlled fixture mux
- 2026-08-29 | G2 | 0e92b6b | fw6.02 | pass: H.264 High and HEVC Main 1080p decoded/presented 90/90 with AAC and zero-copy native output | results/G2/PPSA88009-20260829-125638-codec-receipts.txt | begin G3 resilience work
- 2026-08-29 | G3 | 9463577 | fw6.02 | pass: unsupported AAC was disabled without losing H.264 video, then HEVC/AAC reopened and completed cleanly | results/G3/PPSA88013-20260829-143003-codec-receipts.txt | add grouped fallback and timed cancellation
- 2026-08-29 | G3 | 249bfef | fw6.02 | partial-pass: all six resilience scenarios completed without a crash, but strict validation exposed that cancellation drain status was incorrectly reported as cleanup failure | results/G3/PPSA88014-20260829-145909-resilience-receipts.txt | distinguish playback stop from resource cleanup
- 2026-08-29 | G3 | 51762cb | fw6.02 | pass: mid-playback HLS failure selected the second URL, timed HEVC cancellation and four repeated H.264/HEVC sessions completed, every cleanup result was zero, and the title returned to launcher | results/G3/PPSA88015-20260829-151056-resilience-receipts.txt | begin bounded VP9 WebM delivery
- 2026-08-29 | G4 | 611765f | host | pass: 39 unit tests, 6 tooling tests, lint, signed SELF/runtime checks, and all 90 controlled WebM frames parsed | docs/HANDOFF.md | run strict VP9 cycle
- 2026-08-29 | G4 | 611765f | fw6.02 | transport-failure: first PPSA88017 launch reached LaunchFlowError before eboot under stale LVD registration | results/G4/PPSA88017-20260829-161014-result.json | retry identical bytes once
- 2026-08-29 | G4 | 611765f | fw6.02 | pass: VP9 Profile 0 at 1080p/1440p/2160p decoded and presented 30/30 each, zero-copy and all cleanup zero | results/G4/PPSA88017-20260829-161418-g4-vp9-receipts.txt | run controller/cache gate
