# Third-party notices

The offline pairing QR code uses Nayuki's QR Code generator, version 1.8.0,
commit `720f62bddb7226106071d4728c292cb1df519ceb`, under the MIT License.
The unmodified source and full license are in `vendor/qrcodegen/`.
Source: https://github.com/nayuki/QR-Code-generator/tree/v1.8.0/c

## Credits and acknowledgements

Special thanks to [JMUtechnologies](https://github.com/JMUtechnologies) for
extensive testing, valuable feedback, and providing IPTV credentials for testing.
Their help has been instrumental in improving ProsperoTV's playback compatibility
and reliability.

ProsperoTV builds on and acknowledges:

- [PS5 Native App Boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate),
  [PS5 Payload SDK](https://github.com/ps5-payload-dev/sdk),
  [PacBrew](https://github.com/ps5-payload-dev/pacbrew-repo),
  [MkPFS](https://github.com/PSBrew/MkPFS), and
  [UFS2Tool](https://github.com/SvenGDK/UFS2Tool).
- [iptv-org/iptv](https://github.com/iptv-org/iptv) for the public channel
  catalog and metadata.
- [ProsperoRadio](https://github.com/blackbearreloaded/ProsperoRadio) as the
  primary native application and user-experience reference.
- [IPTVnator](https://github.com/4gray/iptvnator) and
  [Megacubo](https://github.com/EdenwareApps/Megacubo) as IPTV product
  references.
- SDL2 (John Törnblom's [PS5 port](https://github.com/ps5-payload-dev/SDL)), RmlUi, FreeType, SQLite, zlib, LLVM, GoogleTest, Montserrat, Noto,
  DejaVu, and Source Han Sans.

## Native build dependencies

The application build uses LLVM/Clang/lld, zlib 1.3.2, and the public
[PS5 payload SDK](https://github.com/ps5-payload-dev/sdk). The bootstrapper
downloads SDK v0.42 after verifying SHA-256
`8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da`.
It downloads zlib 1.3.2 from the upstream source archive after verifying
SHA-256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`
and compiles its static archive locally. Both dependencies remain under ignored
`.deps/native/`, retain their upstream licenses, and are not distributed by
this repository. No Sony SDK file is included.

Target C++ compilation uses the LLVM libc++ headers distributed by the public
SDK. Those headers retain the Apache-2.0 WITH LLVM-exception license recorded
upstream. The application does not redistribute or dynamically load the
complete libc++ or libc++abi archives.

The PS5 ELF converter and FSELF writer in `tooling/native/` are derived from
[SharpProspero](https://github.com/SvenGDK/SharpProspero), Copyright (C) 2026
SvenGDK, GPL-3.0, and were translated to C++ and modified by BlackBearReloaded.

## MP2 audio decoder

MPEG Layer II audio uses [minimp3](https://github.com/lieff/minimp3), pinned to
commit `ea99364f61c14656440e8d77e9c233ccf3124633`, under CC0-1.0.
The source and license are included in `vendor/minimp3/`. Native AAC decoding
remains the preferred path for AAC-LC mono/stereo.

## Software audio fallback

AAC Main, multichannel AAC, AAC-LATM, AC-3 and E-AC-3 use the audio-only
[FFmpeg](https://ffmpeg.org/) 8.0.1 decoder and stereo downmixer (libavcodec,
libavutil and libswresample), under LGPL-2.1-or-later. The build downloads
the upstream source archive, verifies SHA-256
`05ee0b03119b45c0bdb4df654b96802e909e0a752f72e4fe3794f487229e5a41`,
and builds static libraries under ignored `.deps/ffmpeg-audio/`.
`tools/setup-audio-dependencies.sh` contains the reproducible configuration
and the PS5 portability adjustment. FFmpeg's upstream license files remain
in `.deps/ffmpeg-8.0.1/`; no proprietary decoder is redistributed.

## The next interface (`opengl-ui/`)

The interface in `opengl-ui/` is not part of a release yet. Its build adds:

- [ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui)
  (GPL-3.0-or-later) and the
  [ps5-opengl](https://github.com/blackbearreloaded/ps5-opengl) SDK it draws
  with, both fetched at pinned versions when it is built; nothing of either is
  kept in this repository.
- [PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon), created
  by ArkSama, MIT. The build fetches the cooperative owned-root helper from
  [mpereiraesaa's fork](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon) at
  commit `54a095c0f19161825e845daa760a03b446e654fa`, builds it unmodified for
  the app's title with the PS5 Payload SDK v0.40 that upstream requires, and
  packages it as `lapy.elf` with `licenses/Lapy-MIT.txt`. The client that
  talks to it (`opengl-ui/ps5/src/elevation/`) and the update kit
  (`opengl-ui/ps5/src/update_kit/`, `opengl-ui/ps5/update_helper/`) come from
  the PS5 Native App Boilerplate and ProsperoEden, GPL-3.0-or-later.
- [miniz](https://github.com/richgel999/miniz) 3.0.2, MIT, in
  `opengl-ui/ps5/third_party/miniz/` with its license and the hashes of its
  files: the self-update helper unpacks releases with it.
- [curl](https://curl.se/) (curl license), [OpenSSL](https://www.openssl.org/)
  (Apache-2.0), [libpsl](https://github.com/rockdaboot/libpsl) (MIT),
  [zlib](https://zlib.net/) (zlib license) and
  [Zstandard](https://github.com/facebook/zstd) (BSD-3-Clause), linked from
  the PacBrew prefix described below: every request the interface makes, and
  the check of the catalog's signature.

## Host test dependency

The host unit-test target downloads
[GoogleTest](https://github.com/google/googletest) 1.17.0 after verifying
SHA-256 `65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c`.
It remains under ignored `.deps/test/`, retains its BSD-3-Clause license, and
is not linked into any PS5 application, runtime, or package artifact.

## Optional PacBrew dependencies

When selected through `PACBREW_*` build variables, the build downloads the prebuilt ports image
from [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)
release `v0.40.2`, verifies its published SHA-256, and extracts only the
`target/user/homebrew` prefix under ignored `.deps/pacbrew/`. It does not
replace the pinned SDK or install files globally. PacBrew recipes and every
linked third-party library retain their upstream licenses; applications must
review those terms before redistribution.

## Optional UFS2Tool dependency

When `.ffpkg` output is requested, the platform bootstrapper fetches
[SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) at commit
`b5307a60d5b4e3a68ba680e0e33cfadf05017c77` into the ignored
`.deps/UFS2Tool` cache and builds it with the host .NET SDK. UFS2Tool is
BSD-2-Clause software and is not distributed by this repository.

## Optional MkPFS dependency

When `.ffpfsc` output is requested, the platform bootstrapper fetches
[PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) at commit
`6cb8313dfe0c988ac52617794553f343243d3a56` into the ignored `.deps/MkPFS`
cache and installs its Python dependencies into an ignored virtual environment
there. MkPFS and its dependencies retain their own licenses and are not
distributed by this repository.

## Independently authored runtime shim

`tooling/native/libc_builder.cpp` and the manifests under
`tooling/native/runtime/` are independently authored for this project and
licensed under GPL-3.0-or-later. The generated `runtime/libc.prx` contains
project-authored compatibility stubs, startup code, and semantic loader
metadata. It contains no Sony runtime implementation.

Original ps5-native-app-boilerplate code is Copyright (C) 2026
BlackBearReloaded and licensed under GPL-3.0-or-later. Source and script files
carry matching SPDX identifiers.

## Original presentation assets

The BlackBear icon, selection artwork, and default selection track
`sce_sys/snd0.at9` are original assets supplied by BlackBearReloaded, Copyright
(C) 2026 BlackBearReloaded, and distributed under GPL-3.0-or-later. The track
is titled `Night Drive`.

No proprietary runtime module, encryption key, or game file is included.

## Noto Sans CJK (new interface, `opengl-ui/`)

Channel names in Chinese, Japanese and Korean are drawn with glyphs baked from
Noto Sans SC and Noto Sans KR, Copyright 2014-2021 Adobe (http://www.adobe.com/),
with Reserved Font Name 'Source'. They are licensed under the SIL Open Font
License, Version 1.1; the license text ships with the app as
`assets/fonts/NotoSansCJK-LICENSE.txt`. The font files come from
https://github.com/notofonts/noto-cjk at a pinned commit when the app is built
and are not stored in this repository.
