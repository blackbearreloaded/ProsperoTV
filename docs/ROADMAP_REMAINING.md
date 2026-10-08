# Remaining roadmap implementation

The active request covers every remaining item in README's roadmap and a pull
request after implementation and validation. The sixteen source, browsing and
guide items recorded in ROADMAP_IMPLEMENTATION.md remain part of the regression
baseline. This record does not mark an item complete until its application path
and relevant checks work.

| Requirement | Required behavior | State / evidence |
| --- | --- | --- |
| Local TV sources | Add HDHomeRun and Tvheadend servers, browse and play their channels | Implemented; host checks, PS5 build and console fixture playback pass; physical tuner untested |
| Phone source management | Add and edit saved playlists/accounts through the paired browser | Implemented; HTTP, persistence, mobile browser and native paired-source acceptance pass |
| Zapping | Next, previous and previously watched channel during playback | Pending |
| Playback channel list | Select a channel from a list over the playing video | Pending |
| Channel banner | Brief channel/guide banner on tune and on request | Pending |
| Audio/subtitles | Select available language tracks and render subtitles | Pending |
| Sleep timer | Stop playback at a selected deadline | Pending |
| Live pause/rewind | Pause and replay several minutes of the current live channel | Pending |
| Deinterlacing | Preserve field-rate motion on interlaced broadcast video | Pending |
| HDR | Preserve HDR metadata and output HDR on compatible displays | Pending |
| Multiview | Two or four simultaneous channels, within measured decoder limits | Pending |
| Parental controls | PIN-protected adult categories and kids-only mode | Pending |
| Profiles | Separate sources, favorites and history by signed-in console user | Pending |
| Interface languages | Follow the console language for menus | Pending |
| Backup/restore | Export sources/favorites/settings to USB and restore them safely | Pending |
| Failure reports | Export a useful redacted diagnostic report to USB in a normal build | Pending |

Completion also requires host checks, a PS5 build, bounded testing on an idle
192.168.4.30 or 192.168.4.40, an updated deliverable, and a pull request. Console
tests retain the workspace lock and sandbox-only test-title protocol.

## Source additions

The source menu and paired phone editor accept HDHomeRun and Tvheadend addresses.
HDHomeRun reads the tuner's `lineup.json`; DRM entries are skipped. Tvheadend
reads `/playlist/channels` and `/xmltv/channels`, with an optional HTTP Basic
account. The server must allow Basic authentication when an account is used.
Credentials remain on the configured server origin for guide, preview and
foreground playback, including playlists that refer to another server.
The existing decoder's codec limits still apply to local streams.

Protocols follow [SiliconDust's HTTP guide](https://www.silicondust.com/hdhomerun/hdhomerun_http_development.pdf)
and [Tvheadend's URL documentation](https://github.com/tvheadend/tvheadend/blob/master/docs/markdown/url.md).

The phone editor uses the existing pairing cookie and request header. It adds,
edits, selects and removes saved sources while the TV is in its menu. Saved
account passwords are never returned to the browser; leaving the password blank
preserves the saved value. Updates during a source refresh or account form are
rejected. No new service or dependency is used.

Validation so far: 115 UI tests under ASan/UBSan, 73 core tests, 13 paired-remote
HTTP tests, the PS5 cross-build, and a headless Edge check of source editing,
password preservation and the phone layout. The latter image is retained locally
at `results/roadmap/phone-sources.png`. These do not claim real tuner testing.

- 2026-10-08 | PR10 | 9ea8ee1 | .30/PPSA88262 | failed: stopped buffered playback reopened and lost its counters | results/roadmap/console-09 | validate stop-before-reconnect fix

The direct-stream path now honors Stop after draining buffered input, before
opening another live session. All 30 tooling/lifecycle checks pass, including
the production EOF branch exercised with a stop during drain.

- 2026-10-08 | PR10 | 74cd24f | .30/PPSA88263 | pass: phone source actions, local TV video/audio, stop and teardown | results/roadmap/console-10 | merge after CI

The console case paired through the displayed code, added both local TV types,
renamed the authenticated account without resending its password, selected a
source and removed another. The fixture required authentication for the
Tvheadend playlist, guide and video. Each ten-second playback presented 231 video
frames and decoded 469 audio frames, then reported the requested stop and clean
cleanup. Installed executable hashes matched the frozen candidate, the title
closed, and all three development services remained healthy. This validates
synthetic services on the console, not physical tuners or real provider accounts.
