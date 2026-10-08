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
| Audio/subtitles | Select available language tracks and render subtitles | Pending |
| Sleep timer | Stop playback at a selected deadline | Implemented; 134 UI sanitizer tests and PS5 build pass; console case pending |
| Live pause/rewind | Pause and replay several minutes of the current live channel | Pending |
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
the script's report had already been collected. Native next/previous/last,
list selection, display capture and compositor cost remain unverified. The next
case must verify fixture health before launch and use a timed remote sequence.
