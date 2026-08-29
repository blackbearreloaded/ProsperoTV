# psiptv handoff

Updated: 2026-08-29 America/New_York

## Current candidate

- Goal: `G1` native shell and catalog startup acceptance.
- Source commit: `799eafa`.
- Title: `PPSA88000`, version `01.000.001`, firmware target 6.02.
- App folder: `dist/PPSA88000/`.
- FFPFSC: `dist/PPSA88000.ffpfsc`.
- Host gates: 25 unit tests, 5 tooling tests, lint, native build,
  signed-container integrity, MkPFS creation, and MkPFS verification passed.

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `eboot.bin` | 5,413,829 | `472305ebf43c2a8a3f2838d22afcfd6fe2d8cc713f7d9abc5ec007fff0d98d62` |
| `sce_module/libc.prx` | 1,284,674 | `e6ff45d16adf687855cc3b33b0c8a4132b6504360b221e0a34c7e99fb3ba0036` |
| `sce_sys/param.json` | 883 | `2acfea4e4f7957e254f07d989bb5b97b92e1d4064467c24109befaa73990d856` |
| `sce_sys/icon0.png` | 355,883 | `282ede549c8118855fc1e1a808703f13ccb1809fdaaaa6f287ac8f2977f053cf` |
| `sce_sys/pic0.dds` | 8,294,548 | `56a982da83853ffce3cf62f6cc169016a0ad4479a3e6082a2b6390eebdbd264b` |
| `PPSA88000.ffpfsc` | 37,486,592 | `9d576bf2542988a6e739237c75b5ea23f1499d1c85b79279e60ecce4a5a149fb` |

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
- 2026-08-29 | G1 | 799eafa | host | pass: all host gates; exec LOAD 0x3fa430; packaged exact candidate | docs/HANDOFF.md | run locked startup case
