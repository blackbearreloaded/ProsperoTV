# Source, browsing and guide roadmap

The development branch implements the sixteen requested items. This document
describes the implemented contracts; it does not claim that a release has been
published or that every provider has been tested.

## Sources and categories

Sources keeps multiple playlists, Xtream accounts and Stalker/Ministra MAC
portals. Cross selects a saved source, Triangle edits it, and the add rows create
another source. Each source has its own cache. Square cycles daily, weekly and
manual refresh; a missing cache is downloaded once even in manual mode.
Options explicitly refreshes the selected list. The guide and on-demand lists
follow the same source schedule.

Touchpad in Live TV opens provider categories. Cross descends into a parent
or opens its channels; Browse all includes descendants. Circle returns to the
parent. Square hides or shows a category, including its descendants. Hidden
entries remain in this management sheet so they can be restored. Visibility
applies to browsing, favorites, search and the guide.

Provider parent IDs and category names separated by `/`, `|` or `::` form
hierarchies such as US → Sports → Football. A name without hierarchy metadata
remains one category; the app does not guess a country's parent from its name.

MAC portals take the provider's portal address and MAC code. The app performs
the provider handshake and profile check, reads live genres and channels,
and obtains a fresh playback URL with `create_link`. Commands returned by the
portal are parsed as data. MAC cookies and bearer tokens stay on their origin
when a request redirects. Provider-specific device registration requirements
beyond the standard MAC profile are not emulated.

## Browsing and favorites

PNG and JPEG logos appear in channel tiles and the large television; failed
artwork falls back to initials. Downloads and decoded texture memory are bounded.

Favorites supports 65,536 channel IDs. Touchpad in Favorites opens folders.
Triangle creates a named folder, Square adds or removes the focused channel,
and Cross opens the folder's list. Membership survives source switches and
restarts. Existing favorites migrate without changing their IDs.

Settings contains Hide channels that failed, Start on the last channel and
Live previews. A preview starts after 1.2 seconds of stable focus, without
audio. It stops when focus changes, a sheet opens, the source changes, or
normal playback begins. Its independent HTTP connection cannot cancel a guide
or source download. Preview failures do not change channel health or history.
The native decoder supplies NV12/P010 pictures; a bounded 640×360 CPU conversion
updates one OpenGL texture on the renderer thread, at up to 12.5 frames/second.
Preview supports the existing H.264/HEVC MPEG-TS and unencrypted TS-based HLS
paths, including MAC portal URL resolution.

Startup playback uses the last selected live channel and its saved source,
unless it is hidden or its recorded playback failed. Input during startup
cancels automatic playback. Movie episodes and
archived programmes do not replace the remembered live channel.

## Programme guide

M3U `url-tvg` / `x-tvg-url` and Xtream's XMLTV endpoint supply the guide. Both
plain XMLTV and gzip are supported. Matching uses channel IDs, then unambiguous
names. Now/next appears beside the channel; ordinary search also matches the
programme currently on air.

R3 opens the guide. D-pad moves through channels and programmes, L1/R1 changes
the day, L2/R2 changes the channel page, Square returns to now, Triangle searches,
and Options refreshes the guide. Cross watches the live channel or a selected
past programme when the provider advertises a compatible archive. Supported
archive forms are Xtream timeshift and M3U `default` / `append` catch-up URLs.
Future programmes cannot be played. Time offsets and archive retention are
checked before constructing playback URLs.

The guide retains a bounded fourteen-day window in either direction, with
limits of 500,000 programmes and 128 MiB of text. Failed refreshes preserve
the last good cache. External XML entities are not expanded.

## On demand

Select an Xtream account, then open On demand. Movies and TV shows have
provider categories, including subcategories and the source's visibility
choices. Shows open seasons and numerically ordered episodes. Triangle searches
the current list; Options refreshes it. Lists and episode catalogues have
separate saved caches, loaded before a scheduled refresh.

MP4 and Matroska H.264/HEVC packets are remuxed into the existing MPEG-TS player.
HTTP byte ranges let MP4 metadata be read from either end of a file. Video is
still decoded by the native backend; supported audio uses the existing audio
pipeline. DRM, encrypted HLS, fragmented-MP4 HLS, unsupported video codecs,
and multiple audio/subtitle selection remain outside this implementation.
The On demand provider API currently supports Xtream, while MAC portals provide
live channels.

## Validation

- All 112 host UI tests pass under ASan and UBSan, including nested category visibility,
  multiple-source persistence, guide matching/timezones/catch-up, portal header
  scope, VOD navigation/cache failures and preview cancellation/frame ownership.
- The 72 core tests and native/tooling regressions pass. Three container tests
  generate synthetic H.264/AAC MP4 and Matroska plus HEVC MP4,
  then feed their real compressed packets through the production demuxer and
  stream parser. Generated media stays under ignored `build/`.
- The PS5 cross-build includes the complete interface and native preview path.
- Host renders cover categories, guide, movies, seasons, episodes, Settings and
  a synthetic preview inside the television. These renders are not hardware
  evidence.
- Console validation used idle console `192.168.4.30`, disposable title
  `PPSA88261` and its own sandbox, with elevation disabled throughout. Both
  runs ended with no active user title and all three development services
  answering. Evidence is retained locally in `results/roadmap/console-01/`
  and `results/roadmap/console-02/`.
- The first run (`50c7bad`) verified HLS/TS previews inside the television,
  logos, nested categories, now/next, guide/archive navigation, programme search,
  source switching, movie/season/episode browsing and native MP4/Matroska playback
  with AAC. It exposed an unresponsive stop while buffered video drained.
- The corrected candidate (`bf5b95d`) keeps polling controls while read-ahead
  and native video queues drain. A completely buffered ten-second MP4 and a
  longer Matroska movie stopped at 5.042 seconds; an episode stopped at 4.042
  seconds and portal live playback at 5.025 seconds. All reported presented
  video, decoded audio, the requested stop and successful cleanup. Preview
  diagnostics reported delivered video, zero decoded audio and successful
  cleanup; menu samples averaged about 16.7 ms per frame.
- The second script's initial source-switch input arrived during the startup
  animation, so its playlist/guide screenshots are not acceptance evidence.
  Those paths are covered by the first run; the second validates the buffered
  stop correction and repeats VOD and portal playback.
- No real provider credentials were supplied. Protocol compatibility is tested
  with synthetic responses, not claimed for every provider.
