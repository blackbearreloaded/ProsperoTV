# psiptv handoff

Updated: 2026-08-29 America/New_York

## Current candidate

- Goal: `G1` native shell and catalog startup acceptance.
- Source commit: `59fe40c`.
- Title: `PPSA88000`, version `01.000.001`, firmware target 6.02.
- App folder: `dist/PPSA88000/`.
- FFPFSC: `dist/PPSA88000.ffpfsc`.
- Host gates: 25 unit tests, 5 tooling tests, lint, native build,
  signed-container integrity, MkPFS creation, and MkPFS verification passed.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `eboot.bin` | 5,414,002 | `7dff12be25aacfb0f8be06ffd8f431413cb88e26b2aea9afe182f24acc2d6a73` |
| `sce_module/libc.prx` | 1,284,674 | `e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036` |
| `sce_sys/param.json` | 883 | `2acfea4e4f7957e254f07d989bb5b97b92e1d4064467c24109befaa73990d856` |
| `sce_sys/icon0.png` | 355,883 | `282ede549c8118855fc1e1a808703f13ccb1809fdaaaa6f287ac8f2977f053cf` |
| `sce_sys/pic0.dds` | 8,294,548 | `56a982da83853ffce3cf62f6cc169016a0ad4479a3e6082a2b6390eebdbd264b` |
| `PPSA88000.ffpfsc` | 37,486,592 | `6b15747be63fb39342fc42a5a2db195bc80d220eb38f6945ceaf5189129d9155` |

## Next bounded case

- Acceptance: ShadowMount registers the exact image; launcher reaches a stable
  rendered psiptv UI; title-specific klog shows no crash; title closes cleanly;
  declared services remain healthy.
- Control: PSRadio or the protocol runner's established launcher lifecycle if
  environment health is ambiguous.
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
