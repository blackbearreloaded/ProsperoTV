# ProsperoTV's interface

ProsperoTV's interface since 01.000.020, built on the
[ps5-homebrew-ui](https://github.com/blackbearreloaded/ps5-homebrew-ui) kit:
frosted panels over a red sky, a hero for the channel in focus, a scrolling
grid with the alphabet beside it, a search drawer, a tuning screen that runs
until a channel's first picture, and an update that installs itself when a
newer version is listed on homebrew.page. It is drawn with OpenGL (ps5-opengl
SDK) instead of the earlier versions' SDL and RmlUi; the catalog, the stores and
the player are the same sources as before, used from the folder above.
It asks for filesystem access the way ProsperoEden does, keeps its files in
`/data/prosperotv`, and makes every request with libcurl.

**This is the released app.** A release is this folder built at the tagged
commit, run on consoles and attached to the GitHub Release. The repository's
root `Makefile` still builds the interface of 01.000.015 and earlier, which
shares those sources and keeps their tests; nothing in this folder is built by
it or by the workflow.

The kit is a dependency fetched when something is built; none of its code is
kept in this repository. It compiles against two source trees:

| Needs | Default path | Override |
| --- | --- | --- |
| `ps5-homebrew-ui` (renderer, themes, components, font files, sounds) | fetched at its pinned commit into `.deps/` by `tools/fetch-kit.sh` | `KIT=` |
| ProsperoTV (catalog, stores, network, player) | the repository this folder is in | `TV=` |

```bash
tools/run-tests.sh                 # the logic and the interface, no OpenGL, under ASan + UBSan
tools/host-snapshots.sh            # build for the PC, run the walk, write build/snapshots/*.png
HOST_SANITIZE=1 tools/host-snapshots.sh build/snapshots-sanitize
ps5/assemble.sh                    # make the console build tree beside the repository
make -C ../../prosperotv-ui-build  # dist/PPSA99003 (folder and zip)
TV_TEST_TITLE=PPSA88021 ps5/assemble.sh   # the same as a disposable title beside the released app
TV_DEBUG_TRACE=1 ps5/assemble.sh          # the app with its diagnostic log always on
```

## The diagnostic log

Settings has a switch, **Diagnostic log**, off by default. While it is on the
app appends what it does to `/data/prosperotv/logs/debug-trace.txt` (the
title's `/download0/prosperotv/` without filesystem access), each line with
the seconds since the app started:

- what the viewer pressed and where the interface was, every tab, list and
  search, every notice and failure dialog, settings as they change;
- each update of the channel list with its network result, and each update
  offer;
- for every channel opened: its name, where its addresses point with the user
  name, password, path and query taken out, how it went and why, the player's
  whole receipt (container, codec, profile, picture size, decoder results,
  transport packets lost), and, when it did not play, what the first bytes of
  each address look like;
- every call the decoders make to the system with its result, and the
  player's own messages.

The file is made to be sent to someone else: no address is written whole.
Lines wait in memory and a thread of their own writes them
(`ps5/src/tv_diag.cpp`), so the menu does not stall on the drive; a file past
four megabytes becomes `debug-trace.prev.txt` at the next launch. `tv/diag.hpp`
is the one call the code makes (`diag::event`), and costs a branch while the
switch is off. `TV_DEBUG_TRACE=1` builds the app with the log on whatever the
switch says (its About page says "debug trace"); the test title turns it on
with a `dev/diagnostics.txt` beside the app.

The test title also reads scripted runs (`ps5/src/tv_dev.hpp`), which
`tools/console-run.py <console address> <app folder> <results> <script>`
drives; the scripts are in `ps5/scripts/`.

## What is here

```
src/tv/               the interface and its logic (namespace ptv), as the console builds it
  model.*             sources, catalog, groups, search, favorites, playback requests: no drawing
  catalog_index.*     what a list is browsed by, worked out once per catalog on the download thread
  channel_text.*      what a record says: display name, picture size, monogram
  platform.hpp        what the logic asks of the machine (threads, clock, network)
  settings.*          reduce motion, sounds, menu sharpness
  theme.*             Dusk as kit tokens, and the sky behind everything
  draw.*              channel artwork, tiles, chips, the mark, names the fonts can write
  browse_screen.*     Live TV and Favorites: hero, list chips, grid
  search_sheet.*      the search and filter drawer
  sources_screen.*    the three sources and their state
  update_sheet.*      a newer version: the offer, the download, the hand-over
  app.*               tabs, status, the Settings and About pages, hints, the failure dialog, notices
host/                 PC renderer, the scripted walk, stand-ins for keyboard and network
tests/                GoogleTest: the logic, the interface under scripted and random input
ps5/                  the console entry point, runtime pieces, and the build-tree assembly
tools/                the builds, the font bake, the sample playlist
```

## How the console build is put together

`ps5/assemble.sh` writes a build tree outside both repositories:

- the **kit's build recipe** (its `tooling/`, `tools/build.sh`, linker script and
  module writer, the wrapped heap), which every OpenGL title of ours uses;
- **ProsperoTV's sources** except the RmlUi interface: `iptv_app.cpp`, the old
  `main.cpp`, the bitmap font engine and `ui/` are left out, and SDL, RmlUi and
  FreeType are no longer linked;
- the **kit files** listed in `ps5/kit-files.txt` under `src/kit/`;
- this folder's `src/tv/` and `ps5/src/` (the entry point, `tv_platform.cpp`,
  the heap and the runtime shims);
- fonts baked by `tools/bake-fonts.sh` and the kit's `glass` sound set.

`ps5/patch_tree.py` then makes the few edits the copies need (the keyboard
without SDL, the import libraries the public SDK lacks, the audio decoders,
the paths of the app's files, libcurl in place of the system's HTTP library)
and fails if any of the text it replaces has changed. Once the app folder is
assembled, `tools/package-extras.sh` builds the two programs the app sends to
the console's payload loader and puts them beside `eboot.bin`: `lapy.elf` and
`self-updater.elf`.

`ps5/src/main.cpp` alternates the two owners of the display: it opens the menu
(EGL display, renderer, controller, sounds), runs it until a channel is chosen,
closes all of it, plays the channel with ProsperoTV's own player, and opens the
menu again where it was. The catalog, the filters and the focus live in
`ptv::Model`, which outlives every menu session.

## Filesystem access, and where the files are

First thing in `main`, `ps5/src/tv_storage.cpp` asks for filesystem access
through upstream Lapy (`ps5/src/elevation/README.md`): a resident Lapy service
if one answers, otherwise the one-shot helper packaged for exactly this title,
sent to the payload loader on the console itself. Nothing else is needed on
the PC or on the network.

| | With access | Without it |
| --- | --- | --- |
| Settings, sources, favorites, recent channels | `/data/prosperotv/config/` | `/download0/` |
| Downloaded channel lists | `/data/prosperotv/cache/` | `/download0/` |
| `app.log`, the player's receipts | `/data/prosperotv/logs/` | `/download0/prosperotv/` |
| The app's own files | where the console mounts the app | `/app0/` |

What the released app kept in the title's storage (sources, favorites, recent
channels, an account, settings) is read before access is asked for and written
to `config/` the first time, so nothing has to be set up again. Without access
the app behaves as before, in its sandbox.

The system's HTTP library refuses every public certificate once the process
has filesystem access, so the catalog and the player use libcurl with OpenSSL
(`ps5/src/tv_http_curl.cpp`), checked against the console's own list of
certificate authorities. The system modules the app needs later (the video and
audio decoders, the keyboard) are loaded before access is asked for.

## Updates

Once per launch the app asks homebrew.page whether a newer ProsperoTV is
listed (`ps5/src/update_kit/README.md`). A newer release opens a dialog over
the menu: **Update now** downloads the release, shows how far it is and how
long it will take, unpacks it beside the app, and closes the app so the new
files can take the old ones' place; **Later** leaves everything alone until
the next launch. A cancel or a failure changes nothing. The viewer's files are
in `/data/prosperotv`, so an update never touches them.

## Fonts

The kit bakes printable ASCII. A channel list needs more, so
`tools/bake-fonts.sh` runs the kit's baker over the kit's own font files (Inter,
Montserrat, DejaVu Sans Mono) with its European alphabet: Latin-1, Latin
Extended-A, Greek and Cyrillic.

Chinese, Japanese and Korean names have two faces of their own, baked from
Noto Sans SC and Noto Sans KR (SIL Open Font License; fetched at a pinned
commit by `tools/fetch-cjk-fonts.sh`, never kept in this repository): about
16,900 characters of GB 2312, Big5 and JIS X 0208 with kana, and every Hangul
syllable (`tools/cjk-ranges.py`). This folder's `tools/font-baker/bake_list.cpp`
bakes them, because the kit's baker measures distances to straight and
quadratic edges only and these fonts are drawn with cubic curves. The two
files are 26 and 16 megabytes, so the console loads each one only when the
list on screen has a name or a group that needs it (`Model::uses_east_asian`,
`uses_korean`); `face_for` in `draw.cpp` picks the face a text is written with.

Names in other scripts (Arabic, Hebrew, Devanagari, Thai: they need shaping,
which the kit's text does not do) fall back to what the fonts can write, then
to the playlist's id for the channel (`shown_name` in `draw.cpp`).

## What has run on a console

On two consoles with system software 6.02: the menu at 3840 x 2160 and 60
frames a second, the letters, pages on a held trigger, every tab, the
hand-over between the OpenGL menu and the player (the decoders' system modules
are loaded and woken before filesystem access is asked for, and the player
accepts that AGC is already initialised), 1080p H.264 channels with sound, the
tuning screen, and the app closing itself after a scripted run.

With filesystem access, on both: access granted by the packaged one-shot Lapy
helper, the files in `/data/prosperotv`, the favorites and recent channels of
the sandboxed version carried over, the channel list downloaded and a channel
played over libcurl.

The update, with a test title and a release made for the test: the offer, the
download, the unpacking, the app closing itself, every installed file replaced
by the newer version's, and the newer version starting with the same data.

Not verified: a 4K channel (the ones tried were refused by their providers),
the system keyboard under a script, how the home screen picks up the new
artwork, an update listed by the real catalog (01.000.020 is the first
release that has the updater, so the one after it will be the first offered),
and the app on a console without a payload loader.

A picture made on a PC shows what the code draws, not the frame rate, the
memory use or the sound of a console.
