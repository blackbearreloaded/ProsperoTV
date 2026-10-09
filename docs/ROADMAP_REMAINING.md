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
| Local TV sources | Add HDHomeRun and Tvheadend servers, browse and play their channels | Implemented; host checks and PS5 build pass, console case pending |
| Phone source management | Add and edit saved playlists/accounts through the paired browser | Implemented; HTTP, persistence and mobile browser checks pass, console case pending |
| Zapping | Next, previous and previously watched channel during playback | Implemented; 140 UI sanitizer tests and PS5 build pass; console case pending |
| Playback channel list | Select a channel from a list over the playing video | Implemented; host input/render checks and PS5 build pass; console case pending |
| Channel banner | Brief channel/guide banner on tune and on request | Implemented; mapped guide, timing and rendered image checks pass; console case pending |
| Audio/subtitles | Select available language tracks and render subtitles | Embedded/HLS audio and container/DVB/WebVTT subtitles implemented with host checks; live-provider and native acceptance pending |
| Sleep timer | Stop playback at a selected deadline | Implemented; 134 UI sanitizer tests and PS5 build pass; console case pending |
| Live pause/rewind | Pause and replay several minutes of the current live channel | In progress: history, controls and decoder/subtitle replay; remaining format/timeline handling and native acceptance pending |
| Deinterlacing | Preserve field-rate motion on interlaced broadcast video | Pending |
| HDR | Preserve HDR metadata and output HDR on compatible displays | Pending |
| Multiview | Two or four simultaneous channels, within measured decoder limits | Pending |
| Parental controls | PIN-protected adult categories and kids-only mode | Implemented; 132 UI sanitizer tests, 73 core tests and PS5 build pass; console case pending |
| Profiles | Separate sources, favorites and history by signed-in console user | Implemented; isolation/migration sanitizer tests, PS5 build and console startup pass |
| Interface languages | Follow the console language for menus | Pending |
| Backup/restore | Export sources/favorites/settings to USB and restore them safely | Implemented; sanitizer/build checks and console sandbox-drive workflow pass; physical USB untested |
| Failure reports | Export a useful redacted diagnostic report to USB in a normal build | Implemented; sanitizer/build checks and console sandbox-drive workflow pass; physical USB untested |

Completion also requires host checks, a PS5 build, bounded testing on an idle
192.168.4.30 or 192.168.4.40, an updated deliverable, and a pull request. Console
tests retain the workspace lock and sandbox-only test-title protocol.

## Live pause and rewind foundations

The transport history retains up to five minutes or 512 MiB, whichever limit is
reached first. It indexes one selected video clock, handles timestamp wrap and
provider timeline resets, and seeks to random-access packets when available.
When those flags are absent, it starts earlier so the stream parser can find a
decodable picture. Reads copy bytes while holding the history lock, preventing
network writes from replacing data still being parsed. Expired positions are
reported explicitly.

History storage now uses its own bounded anonymous mapping instead of allocating
512 MiB from the application's 512 MiB heap. Its lifetime remains scoped to the
history owner, and allocation failure leaves it unavailable. All six existing
history tests pass under ASan/UBSan with the mapped storage, and the native
application build passes. Foreground integration and hardware acceptance remain
pending for this change.

Native control primitives pause both workers, redraw the frozen picture for the
overlay, and adjust video pacing on resume. Repositioning releases submissions
blocked by full queues and discards packets from the previous playback
generation. Audio waits for a picture from the new generation before aligning
its timestamps. Foreground integration is now being implemented on the local
timeshift branch; no console installation or functional acceptance is claimed.

The 93-test core sanitizer suite includes six history tests covering fragmented
input, time/byte limits, overwrite detection, clock wrap, multiple programme
clocks, unflagged seeking and concurrent download/playback. All 29 tooling tests
pass, including native full-queue cancellation and audio alignment checks.
The final application-only PS5 cross-build also passes; its local executable
SHA-256 is `4e769dcb7ba6cf7fb65accaf5e10d2e62174ccb4bd4c4260699048bb01fc0de0`.
This checkpoint has not been packaged or installed on a console.
The transport reposition API now restores the retained video's extended clock
and resets parser buffers and subtitle timing. A regression replays forwards and
backwards across two timestamp wraps, including an exact wrap, and checks the
video, audio and subtitle timestamps without reopening the decoder. All 95 core
tests pass under ASan/UBSan on the VOD-fixes baseline. The native application
also compiles and links with the reposition API.

The foreground transport path now copies arriving input into history while the
decoder is paused. It has controller and phone requests for pause/resume,
30-second seeks and return to live, plus a paused/behind-live banner. The
channel and track menus retain their own directional controls; playback-only
phone actions are ignored by the main menu. A provider timeline boundary
expires the old bytes even when the replacement stream uses identical timestamps.
Pause-aware progress checks keep an intentionally frozen picture from triggering
the normal video-stall watchdog. History allocation failure falls back to ordinary
playback.

The stream parser now retains parameter sets by their codec IDs, capped at
64 KiB, and supplies missing sets with the first random-access picture after a
reset. The combined picture and setup must fit the existing access-unit limit.
Real H.264 and HEVC regression fixtures remove the keyframe's headers, pass it
through reposition, then decode it in a fresh host decoder and compare its pixels
with the original. The 97-test core and 20-test media sanitizer suites and PS5
application build pass. Historical configuration retention is described below;
native decoder acceptance is still pending.

Same-timeline subtitle seeks now rebuild the selected decoder from retained raw
packets. Retention covers the five-minute video window plus the maximum cue
duration, while preserving the 2,048-packet and 16 MiB bounds. Repeated transport
and WebVTT packets are deduplicated; provider timeline resets clear old captions.
Text, language/Off selection, DVB bitmap replay and timed clearing pass in the
21-test media/subtitle sanitizer suite, and the PS5 application builds. Ordering
between new provider timelines and subtitles downloaded ahead of demux still
needs validation.

Repeated seek requests now accumulate from the pending target until a picture
from the new decoder timeline arrives. Old generations and frames discarded
during reset cannot acknowledge that target. Controls and parser reposition
share their existing owner mutex; pause/seek input is applied in event order,
and a pause requested after a seek is preserved through the parser reset.
The 99-test core sanitizer suite covers queued/in-flight targets, actual
keyframe acknowledgement, moving retention bounds and arithmetic limits. The
native queue-state host check and full-assembly PS5 application build also pass.
Responsiveness and presentation timing still require console acceptance.

Decoder configuration snapshots now preserve parameter IDs across previously
decoded history. They retain the five-minute window and its preceding setup,
bounded by 256 snapshots and a 4 MiB payload/table budget. Expiry moves the
transport seek boundary with the configuration boundary; a provider reset
cannot apply the old timeline's boundary to new bytes. Snapshot allocation uses
the native build's non-throwing path and failure expires optional history.
Pictures now select their timestamp at the first VCL NAL, preventing delimiters
left from the previous PES from assigning the previous picture's time.

The configuration tests replay both directions across real H.264 and HEVC setup
changes, strip in-band headers, and compare pixels decoded by fresh decoders.
They also cover bounded retention, rejected expired seeks, untimed changes and
provider resets. This applies to compatible decoder formats: resolution/profile
changes still require decoder reopen work.
All 103 core and 21 media/subtitle sanitizer tests and the application-only PS5
build pass at 289d250. No native configuration-replay acceptance is claimed.

Forward seeks now scan skipped retained bytes in bounded chunks before resetting
the decoder. Scanning retains configuration changes without submitting video or
audio, flushes the last pending picture at the target boundary, and yields to
new controls between chunks. Copies carry their timeline generation so a
concurrent provider reset cannot feed bytes from a different timeline. The
watchdog excludes this intentional scan. All 105 core and 21 media/subtitle
sanitizer tests and the application-only PS5 build pass at 1c8e33e; the core
regression also checks a configuration change held in the final pending picture.

This remains incomplete: provider timeline/subtitle ordering, direct WebM live
playback and native pause/rewind/expiry, synchronization
and resource acceptance still need work. The earlier 146-test UI sanitizer suite,
13 phone remote integration tests, native queue-state host check and PS5
application build pass. The foreground changes
have a frozen earlier test package (446ca92, PPSA88273). Case31 launched on .30
and rendered the pairing screen, but its code expired before the playback
controls connected, leaving native timeshift acceptance inconclusive.
No timeshift PR is open.

Case32 installed the forward-scan build as PPSA88274 and paired successfully.
The synthetic playlist played for 155 seconds (3,797 presented pictures,
7,155 decoded audio frames, clean player teardown), but history never became
available. The channel request used the reconnect flag to enable history;
custom playlists intentionally do not reconnect automatically. Live requests
now carry a separate flag, enabled for channel playback and disabled for movies
and archived programmes.
All 146 UI sanitizer tests and the PS5 application-only build pass. The request
tests distinguish a playlist channel, a movie, a past programme and the currently
airing programme. Evidence: `../psiptv/results/roadmap/timeshift-live-result.json`.

Case33 (c6e6afb, PPSA88275) validates native custom-playlist pause/resume,
cumulative rewind, forward seek and return to live. Presentation stayed fixed
for a 20-second pause while history advanced, and remained within 0.72 seconds
of the buffered live position after returning. The app returned to browsing and
exited cleanly with healthy services. All 52 installed file hashes matched.
The receipt has 1,985 presented frames and 3,741 decoded audio frames; three
audio and two video queue underruns occurred across transitions. Their impact,
audio/subtitle synchronization, expiry, changing formats and 4K/HEVC history
remain unverified. Evidence: `../psiptv/results/roadmap/console-33/result.json`.

Native AAC now recreates its decoder when the audio timeline changes, matching
the existing software-decoder reset. The ASan/UBSan state regression covers
success, stale-buffer clearing, delete/create failures and software fallback;
the PS5 application build passes. Case34 (f442b03, PPSA88276) confirms all four
seek commands created fresh AAC contexts, with 1,957 presented pictures, 3,690
decoded audio frames and clean teardown. Three audio/three video queue gaps
remain; audible impact and A/V synchronization are not proven by these checks.
Evidence: `../psiptv/results/roadmap/console-34/result.json`.

Download-history replay foundation (8eb5e7e): a playback parser can now restore
configuration from an independent download parser, without retaining pointers
into its lifetime. Missing, expired and not-yet-scanned positions are rejected
before playback resets. A successful restore replaces stale local configuration
history; older positions still require the download parser's history.
The regression overwrites unread configuration headers in a small transport ring
while playback remains paused, then resumes headerless pictures with the correct
configuration. All 107 core checks pass with explicit ASan/UBSan flags. The real
H.264/HEVC replay test also passes under sanitizers, comparing decoded pixels
across both local and independent-parser restoration.
The application-only PS5 build passes at 8eb5e7e; no new console package was
installed. Evidence: `../psiptv/results/roadmap/timeshift-download-result.json`.
StreamRunner now retains metadata on download (e0c3ec8), before transport bytes
expire. Case35 validates native pause expiry/recovery and basic seeking. Historical
settings also initialize a fresh parser after programme/codec headers expire
(46a2196); 111 core and 21 real-media/subtitle sanitizer tests and the PS5 build
pass. Native changing-format and timing acceptance remain open.

- 2026-10-08 | rewind | 4c63161 | host/PS5 build | partial-pass: mapped history and clock replay | ../psiptv/results/roadmap/timeshift-clock-native-result.json | integrate foreground controls
- 2026-10-08 | rewind | ec377c9 | host/PS5 build | partial-pass: foreground controls | ../psiptv/results/roadmap/timeshift-player-result.json | decoder/subtitle replay, native acceptance
- 2026-10-08 | rewind | 7ef62d8 | host/PS5 build | partial-pass: parameter replay | ../psiptv/results/roadmap/timeshift-parameters-result.json | configuration versions, native acceptance
- 2026-10-08 | rewind | 04eb912 | host/PS5 build | partial-pass: subtitle replay | ../psiptv/results/roadmap/timeshift-subtitles-result.json | timeline ordering, native acceptance
- 2026-10-08 | rewind | b2bf8dd | host/PS5 build | partial-pass: seek/control ordering | ../psiptv/results/roadmap/timeshift-seek-controls-result.json | bounded native TS case
- 2026-10-08 | rewind | 289d250 | host/PS5 build | partial-pass: historical decoder setup | ../psiptv/results/roadmap/timeshift-config-result.json | forward scan and native acceptance
- 2026-10-08 | rewind | 1c8e33e | host/PS5 build | partial-pass: forward configuration scan | ../psiptv/results/roadmap/timeshift-scan-result.json | native acceptance and remaining formats
- 2026-10-08 | rewind | c6e6afb | .30 PPSA88275 | partial-pass: native pause/seek/live controls | ../psiptv/results/roadmap/console-33/result.json | sync, expiry and remaining formats
- 2026-10-08 | rewind | f442b03 | .30 PPSA88276 | partial-pass: native AAC seek reset | ../psiptv/results/roadmap/console-34/result.json | timing, expiry and remaining formats
- 2026-10-08 | rewind | 8eb5e7e | host/PS5 build | partial-pass: independent download configuration replay | ../psiptv/results/roadmap/timeshift-download-result.json | connect producer loop, timeline ordering, native expiry acceptance

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

## USB data handling

Settings offers backup, restore and failure-report export, with a drive chooser
when multiple readable USB mounts are present. The backup is
`ProsperoTV-backup.sqlite3` on the selected drive; the confirmation explains that
it includes provider passwords. It includes source records, visibility choices,
favorite folders, favorites, history and interface settings. Pairing tokens and
downloaded catalogues are excluded.

The menu pauses model and remote mutations, closes the database and runs the
storage operation on a worker while showing progress. Restore checks the complete
bounded archive and checksums before replacing any settings. A rollback journal
recovers the original files after an interrupted restore. The model and settings
are reconstructed before browsing resumes. A failed recovery keeps the journal
and prevents startup from changing the affected files.

A normal build saves the last playback failure as numeric decoder/network
evidence. Successful playback does not erase it. Export writes
`ProsperoTV-failure.txt`, retaining version, time, result and attempt count while
excluding channel names, URLs, provider responses and credentials. USB access uses
the app's existing filesystem access; no extra access request is made by these
actions.

Validation: all 122 UI tests pass under ASan/UBSan, including backup round trips,
corrupt/incomplete archive rejection, interrupted-restore recovery, retained
pairing state, multiple drives, controller confirmation and report redaction.
The PS5 production cross-build also passes. Physical USB validation remains open.

## Console profiles

The user who opens the title owns its profile. Sources, favorite folders, recent
channels, settings, cache, pairing tokens and logs live under that user's folder.
The account service and controllers use the same initial console user. Settings
shows that person's console name. Reopen the app from another console user to use
their profile; the application does not change the console's signed-in user.

The first user to launch this version inherits the old shared settings once.
Original files are retained. Other users start with empty personal libraries,
and phones must be paired to the new profile. Interrupted migration resumes
without replacing existing profile files. Missing user identity or a damaged
migration record stops startup instead of exposing shared accounts.

Validation: 125 UI tests under ASan/UBSan and the PS5 build pass. Profile tests
cover two users, private pairing/settings paths, first-user-only migration,
resuming an interrupted copy and retaining deliberate deletions after migration.
Console profile discovery is supported by the test runner; changing console
users is not automated.

PR #11 validation after rebasing onto main: all 126 UI tests pass under
ASan/UBSan. A filesystem obstruction during restore now has explicit coverage:
the recovery journal survives a failed rollback, recovery succeeds after the
obstruction is removed, and the backup can then be restored successfully.
CI also passed for the rebased application at `60832ed`. Physical USB media and
switching actual console users remain unverified.
- 2026-10-08 | Household | 7d7d225 | PPSA88261 / .30 | pass: profile startup, sandbox-drive backup/restore/report, clean teardown | results/roadmap/console-06/validation.json | remaining roadmap

The console case restored volume from 95 to the backed-up 100 and exported a
numeric report for an intentional HTTP 404. The generic runner returned 1 for
nine expected diagnostics/labels containing `failed`; the explicit case validator
checks those exact flags, the installed executable hash and healthy teardown.

## Parental controls

Each console profile can set a masked four-to-eight-digit parent PIN. Adult
provider flags, adult category paths and manually protected categories apply to
live lists, favorites, search, guide/catch-up, resume, previews and on-demand
playback. Episodes inherit their show's restrictions. Protection locks on launch;
unlock lasts for the current app session, with an explicit lock action.

Kids-only mode permits Kids/Children categories and categories explicitly approved
by a parent, with adult restrictions taking precedence. Provider labels are not
content ratings: the Settings description asks parents to review them. Triangle
in the live category sheet, or Touchpad on an on-demand category, cycles its rule
between normal, PIN required and approved for kids. Rules include subcategories.
Only an unlocked parent can change those rules, edit sources on the TV or phone,
export a credentials backup, restore settings, change the PIN or leave kids mode.

The PIN file stores a random salt and PBKDF2-HMAC-SHA256 result, using the existing
OpenSSL dependency ([derivation](https://docs.openssl.org/3.0/man3/PKCS5_PBKDF2_HMAC/),
[random bytes](https://docs.openssl.org/3.0/man3/RAND_bytes/)). Five wrong attempts
cause a 30-second delay, retained across restart. A damaged file or failed retry
counter write leaves protection locked. PIN input is excluded from diagnostic
input logging. Backups include protection settings and category rules.

Catalog schema 5 preserves provider adult flags. The application downloads old
catalog caches again once, since earlier cache schemas discarded those flags.
Core readers retain backward compatibility. Host validation covers persistent
retry delay, corrupt files, cancelled/mismatched PIN prompts, locked storage and
phone management, provider inheritance and playback policy; native menu/IME and
filesystem validation remains part of the remaining console regression.

## Sleep timer

Settings offers Off, 15, 30, 60, 90 and 120 minutes, with a remaining-time display.
The monotonic deadline belongs to this app session. It survives menu reopening,
stream URL retries and settings restore, and is passed unchanged to the foreground
player. It is not included in saved settings or rearmed on app launch.

Expiry stops foreground playback and muted previews, cancels queued playback and
prevents automatic last-channel resume. Choosing a channel explicitly wakes
playback without rearming the timer. Host checks exercise the exact deadline,
cancellation, menu reconstruction, invalid clocks and the preview/resume behavior;
the real fifteen-minute deadline is also verified on the console.

- 2026-10-08 | PR12 | dbfec0d | .30/PPSA88264 | inconclusive: 45-second fixture ended before sleep deadline | results/roadmap/console-11 | use longer stream
- 2026-10-08 | PR12 | dbfec0d | .30/PPSA88264 | pass: real sleep deadline, menu return and clean teardown | results/roadmap/console-12 | parental IME acceptance

The corrected case used a verified twenty-minute stream and disabled the test
driver's playback timeout. The fifteen-minute timer was armed before playback;
playback stopped after 837 seconds with 20,882 presented frames and 39,254 decoded
audio frames. The final captures show the timer Off and an idle preview after
returning to Live TV. Installed hashes matched, cleanup succeeded, the title
closed, and all development services remained healthy. The generic runner's two
flags were `hide_failed` setting labels; the case validator checks the receipt
and teardown separately. All 135 UI sanitizer tests also pass. Native parental
PIN/IME and filesystem acceptance remain pending.

## Playback navigation and banner

Up/Down switches through the current filtered live list, wrapping at either end;
L1/R1 offers the same previous/next actions.
Square recalls the most recent other channel, including one outside the search,
while respecting hidden categories and parental restrictions. Cross opens the
channel list over the video; Up/Down moves the selection, Left/Right changes page,
and Cross tunes. Circle closes the list before returning to the browser on a
second press. VOD and catch-up do not accidentally switch to live channels.

The channel banner appears for five seconds from the first picture. Triangle or
Touchpad shows it again. Now/next uses the guide's mapped channel IDs and follows
programme changes. The existing Touchpad + R1 statistics chord remains available.
The app reuses its baked fonts and script fallbacks. Text is composited onto a
separate presenter surface, preserving the decoder's reference pictures; bounds
and padding are checked for both eight-bit and native low-bit Main10 surfaces.

Channel switches wait for the previous player's full teardown. Ordinary streams
open directly, while portal channels use the existing cancellable link resolver.
The sleep deadline survives both paths. The host checks cover filtered wrap,
history restrictions, list input, banner expiry, guide changes, Unicode font
rasterization, invalid surfaces and untouched padding. The 140-test sanitizer
suite passes, as does the PS5 build. Host render captures are retained locally as
`results/roadmap/playback-banner.png` and `playback-list.png`. Controller behavior,
compositor cost and uninterrupted teardown on hardware remain console acceptance
criteria.

- 2026-10-08 | Playback | 72c5141 | PPSA88261 / .30 | partial-pass: pairing, 45s HLS, list-open input, clean teardown | results/roadmap/console-07/validation.json | native navigation

The first playback case installed and verified this candidate. The fixture server
started late after a host process-launch error; the script then left focus above
the channel grid. Pairing and a subsequent remote-selected channel worked, but
the script's report had already been collected. The later case below verifies
fixture health before launch and uses a timed remote sequence.

- 2026-10-08 | PR13 | 2162453 | .30/PPSA88265 | pass: next/previous, Up/Down, list selection/close and teardown | results/roadmap/console-13 | physical last-channel and overlay checks

All 141 UI sanitizer tests and the fresh PS5 build pass. The console case paired
through the displayed code and exercised six native playback sessions, switching
Alpha/Beta through next, previous, Up/Down and the playback list. Every session
presented video, decoded audio and completed cleanup. Closing the list retained
playback; the next Back returned to browsing. Installed hashes matched, the title
closed and all development services remained healthy. The generic runner flagged
only the `hide_failed` settings label; the case validator checks all six receipts
and teardown. The phone's Favorite command has a different meaning from the
controller's Square button, so physical last-channel recall, visual overlay
capture and compositor-cost measurements remain unverified.

## Audio selection

Options during playback opens Audio; L1/R1 switches to Subtitles. Up/Down selects a track, Left/Right changes
page, Cross applies it, and Circle or Options closes the panel. The same controls
work for live television, movies and catch-up. Off mutes the selected stream.
The panel shows provider ISO 639 language codes, codec and accessibility labels;
an absent language remains a numbered track. Selection lasts for this playback
session. The channel list and live zapping are suspended while the panel is open.

The transport reader retains up to 32 supported audio PIDs, preserves selection
when a recurring PMT reorders tracks, and falls back when a provider removes the
selected PID. Off remains Off through those updates. MP4 and Matroska remuxing
preserves all supported audio tracks and their language metadata. The existing
AAC, MPEG audio, AC-3 and E-AC-3 codec limits still apply. Separate HLS audio and
WebVTT renditions use the same controls, as described below.

The demux worker applies requests between chunks and publishes a synchronized
snapshot to the controls. The native backend discards and joins the old audio
worker, opens the selected audio decoder and keeps the video decoder and
presentation alive. New audio waits for the displayed timestamp; a bounded
fallback handles broken timestamps. Changing audio cannot hold video behind the
ordinary audio buffering gate. Decoder failures are reported in the panel and a
different track can be selected. Native switch latency and synchronization still
require measurement on hardware.

Validation: 79 core tests, 42 tooling/remote checks, four MP4/Matroska tests and
142 UI tests pass. The parser tests cover PMT reordering/removal, Off, retries,
bounded track copies, malformed descriptors, continuity counters and transport
clock wrap. Real generated two-language MP4 and Matroska fixtures switch audio
while retaining one video backend. The PS5 cross-build and native buffering
state check pass. The rendered panel was inspected at
`results/roadmap/playback-audio.png`. No console installation is claimed for
these changes; the last recorded installed test build remains 72c5141.

## Subtitles

The playback track panel lists embedded subtitle languages and an Off choice,
including forced and hearing-impaired labels supplied by the provider. MP4 and
Matroska expose supported text, SubRip, ASS, WebVTT, mov_text, DVB, DVD and PGS
tracks through the existing FFmpeg dependency. Text keeps punctuation, Unicode,
line breaks and word wrapping; ASS styling and vector drawings are omitted.
Bitmap captions retain their palette, transparency and source-canvas position.
The presenter composites captions onto its scratch surface at the displayed
video timestamp, including native ten-bit surfaces and text above the banner.

Live MPEG-TS and TS HLS segments discover DVB subtitle languages and page IDs
from the provider's PMT. The reader assembles complete bounded PES packets,
preserves partial packets across repeated PMTs, ignores duplicates, and drops
incomplete captions after packet loss. Removed languages disappear from the
panel. Bad subtitle framing and scrambled subtitle PIDs leave clear audio and
video running. Subtitle timestamps use the video's transport-clock epoch.
The framing follows the existing [FFmpeg transport reader](https://github.com/FFmpeg/FFmpeg/blob/n8.0.1/libavformat/mpegts.c).

Only the selected language is decoded. A bounded compressed-packet cache retains
captions downloaded ahead of playback while subtitles are Off, allowing a
language change to recover the caption for the current picture. Cues expire by
video time. Packet, track, rectangle, pixel and queue limits isolate subtitle
failures. Discontinuities discard old captions and bitmap decoder state; track
changes and immutable presentation snapshots are synchronized across threads.

Host checks cover MP4/Matroska subtitle-to-video timing in two languages, actual
DVB palette decoding and clearing, transport fragmentation/loss/clock wrap,
provider updates, language changes, resource limits and eight/ten-bit compositing
without touching frame padding. The rendered menu and English/Chinese captions
were inspected in `results/roadmap/playback-subtitle-menu.png` and
`playback-subtitles.png`. Native timing, language-switch latency and compositor
cost remain console acceptance work. No newer console installation is claimed.

Validation: 83 core tests, 13 media/subtitle tests and 144 UI tests pass under
ASan/UBSan, and the PS5 production cross-build passes. Separate HLS renditions
remain the next part of this feature; these checks do not close that requirement.

HLS master parsing now retains each variant's audio/subtitle group references
and up to 32 advertised renditions, including resolved HTTP(S) URLs, BCP 47
languages, default/forced flags and accessibility labels. It checks duplicate
names/defaults, missing groups, required subtitle URLs and bounded labels against
[RFC 8216](https://www.rfc-editor.org/rfc/rfc8216.html#section-4.3.4.1).
The discovery change passed the 86-test core sanitizer suite, 42 tooling checks
and PS5 build.

The player now gives FFmpeg one selected quality and only its matching audio and
subtitle groups. Each nested request uses the application's HTTP client,
origin-scoped credentials, redirect handling and validated byte ranges. The
reader bounds playlist and WebVTT resources, rejects local-file access and keeps
provider URLs out of FFmpeg diagnostics. HLS without external renditions retains
the existing transport-stream path and its stale-segment recovery.

Separate audio is remuxed alongside the video, retaining all supported languages
so the existing demux worker can switch at the displayed picture after network
read-ahead. This downloads the advertised audio tracks; only the selected track
is decoded. Provider names and BCP 47 language tags remain visible in the panel.
Fragmented MP4 initialization segments and byte ranges are handled by the same
reader without transcoding.

WebVTT resources apply their `X-TIMESTAMP-MAP` before demux, align the wrapping
transport clock with the video, and suppress identical cues repeated in adjacent
segments. Subtitles are enabled before stream probing so initial dialogue is
retained even while the panel is Off. A small, version-checked patch fixes the
pinned FFmpeg reader's subtitle context and AVIO buffer cleanup; generated
multi-segment fixtures cover the lifetime under LeakSanitizer.

Validation: 87 core, 17 media/subtitle and 144 UI sanitizer tests, plus 42 tooling
and remote checks. The generated HLS fixtures cover transport stream, fragmented
MP4, byte ranges, redirected playlists, two audio/subtitle languages, mapped
timestamps, repeated captions and resource cleanup on failure. The PS5 executable
cross-build also passes. Live-provider discontinuities,
native synchronization, selection latency and resource cost remain acceptance
work. These changes have not been installed on a console.

Native baseline acceptance (2026-10-08): candidate `2a46313`, disposable title
`PPSA88266`, passed 12-second playback of synthetic TS, MP4, Matroska and
external-rendition HLS on the verified-idle console `.30`. Each produced
260–281 video frames and 524–563 decoded audio frames, with requested stop,
clean native cleanup and healthy services after title exit. All 52 uploaded
files matched the frozen package. Evidence: `results/roadmap/console-14/validation.json`.
The generic runner flagged the settings label `hide_failed=0`; the scoped
validator checked all four receipts and complete installed hashes independently.
This proves baseline playback only. Track selection, subtitle visibility and
synchronization, switching latency and resource cost remain pending.

- 2026-10-09 | rewind | e0c3ec8 | .30 PPSA88277 | partial-pass: download metadata, 330s expiry/recovery, seek/live, clean teardown | ../psiptv/results/roadmap/console-35/result.json | formats/sync

- 2026-10-09 | rewind | 46a2196 | host/PS5 build | partial-pass: fresh parser restores expired programme/codec headers | ../psiptv/results/roadmap/timeshift-startup-result.json | native 4K/timing

- 2026-10-09 | rewind | 46a2196 | .30 PPSA88278 | partial-pass: 4K HEVC controls/teardown, no heap failures; timing gaps remain | ../psiptv/results/roadmap/console-36/result.json | diagnose pacing

- 2026-10-09 | rewind | 08b8a46 | host/PS5 build | partial-pass: seek refill and stale queue regression | ../psiptv/results/roadmap/timeshift-rebuffer-result.json | native 4K comparison

- 2026-10-09 | rewind | 08b8a46 | .30 PPSA88279 | pass: 4K seek refill; video gaps124->1, late122->1, clean teardown | ../psiptv/results/roadmap/console-37/result.json | formats/metadata/sync

- 2026-10-09 | rewind | 6eb86e1 | host/PS5 build | partial-pass: programme tracks follow historical video PES; 112 core/21 media checks | ../psiptv/results/roadmap/timeshift-programme-result.json | native tracks

- 2026-10-09 | rewind | 72a1254 | .30 PPSA88280 | failed: programme tracks change before queued video; clean teardown | ../psiptv/results/roadmap/console-38/result.json | preserve queued programme media

- 2026-10-09 | rewind | 911f3b2 | .30 PPSA88281 | pass: A/B tracks across pause/seek/live, zero queue gaps, clean teardown | ../psiptv/results/roadmap/console-39/result.json | natural transition

- 2026-10-09 | rewind | 911f3b2 | .30 PPSA88281 | partial-pass: natural track transition; one video queue gap unclassified | ../psiptv/results/roadmap/console-42/result.json | timing/formats

- 2026-10-09 | rewind | f79f9e5 | .30 PPSA88282 | pass: natural transition gap81.5ms, none>250ms; clean teardown | ../psiptv/results/roadmap/console-43/result.json | formats/timeline/subtitle sync

- 2026-10-09 | rewind | 0c13eff | .30 PPSA88283 | failed: replacement decoder discarded input before opening; native-state regression reproduces | ../psiptv/results/roadmap/console-44/result.json | idle decoder cancellation

- 2026-10-09 | rewind | d7db0b4 | .30 PPSA88284 | pass: 360p/720p native reopen, bidirectional history seeks, live continuation, one connection and clean teardown; 115 core/21 media/native-state checks | ../psiptv/results/roadmap/console-45/result.json | codec/timeline/subtitle/WebM coverage
