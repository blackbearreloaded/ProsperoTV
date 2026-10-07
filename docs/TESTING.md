# Testing

The repository separates fast host checks from behavior that only real PS5
hardware can prove.

## Commands

| Command | Scope |
| --- | --- |
| `make test-deps` | Fetch and verify the pinned host-only GoogleTest source. |
| `make test-unit` | Compile and run the host-native GoogleTest application tests. |
| `make test-integration` | Exercise repository scripts through subprocesses and temporary files. |
| `make test` | Run both host test suites. |
| `make check` | Run linting, all host tests, and a complete folder build. |

GitHub Actions runs `make test-unit` and `make test-integration` as separate
Ubuntu steps, so every pull request executes both layers with clear failure
reporting. Host tests must remain deterministic, must never contact a console,
and must be safe to run in parallel with unrelated console work. The first
unit-test run downloads a pinned GoogleTest archive after verifying its
SHA-256; later runs reuse `.deps/test/`.

Run one test or suite with normal GoogleTest arguments:

```bash
make test-unit GTEST_ARGS='--gtest_filter=AssetTextTest.MissingAssetUsesFallback'
```

## Unit-test policy

Write unit tests for reusable logic with meaningful behavior: parsers, state
transitions, bounds handling, input mapping, protocol messages, resource
ownership, and error paths. Keep platform calls behind a small boundary so the
logic can compile and run on Linux without a PS5 or proprietary SDK.

The starter suite in `tests/test_demo_renderer.cpp` uses GoogleTest to validate
fallback, line-ending, truncation, and null-termination behavior for packaged
text assets. GoogleTest is a host-only development dependency: it is never
compiled into `eboot.bin`, `libc.prx`, or a PS5 package.

Do not add tests for trivial constants or one-line drawing calls merely to
increase a coverage percentage. Test observable contracts and regressions.

## Host integration tests

`tests/test_remote.py` compiles the production HTTP server with a small host
input consumer. It checks explicit pairing, expiry, saved browser credentials,
restart/revocation, volume bounds and save failures, all button mappings, Unicode search,
empty and oversized queries, invalid UTF-8, fragmented requests, and concurrent
slow clients. Run it alone with
`python3 -m unittest discover -s tests -p test_remote.py -v`.
For hardware acceptance, open Settings → Pair a phone on the TV, scan the QR,
pair using its code, verify each direction/OK/Back against visible TV changes,
search using the browser keyboard, clear the query, and stop playback with Back.
Confirm reconnection after an app restart and rejection after forgetting phones.
Adjust volume in both UIs, verify they agree, and listen for mute and restored
audio during playback. The host audio regression also checks both native channel
gains, clamping and unchanged-volume suppression with a mocked AudioOut call.
Capture both the phone page and corresponding TV states.
Run `opengl-ui/tools/run-tests.sh` as well: it drives the released interface
with phone input and checks search, keyboard cancellation, filters, and tabs.
Build the released interface with `opengl-ui/ps5/assemble.sh`, then run `make`
in the assembled tree. The repository root builds the legacy interface.
When Remote Play is unavailable, `make IPTV_REMOTE_CAPTURE=1` in the assembled
tree enables an opt-in screenshot hook. Creating
`/download0/remote-capture.request` captures the next frame to
`/download0/remote-capture.bmp` and removes the request (checked twice a second).
For a build with filesystem access, these two files are in its config directory.
Retrieve only completed captures. Normal builds omit this hook entirely.

`tests/test_tools.py` invokes complete repository scripts with temporary input
and controlled environment variables. Use this level for metadata updates,
build orchestration, package validation, and deployment resolution. Network
operations must be mocked or use an explicit dry-run mode; host CI must never
contact a console.

Each test must clean up its files, avoid shared mutable state, and include the
failure case that would have caught the associated bug.

## PS5 integration validation

Rendering, controller input, AudioOut, mounted paths, launch/closure behavior,
and firmware compatibility require hardware validation. A passing host suite
does not prove those properties.

For a hardware milestone:

1. Build an exact candidate from a clean commit and record its digest.
2. Acquire the shared console lock only for the test window.
3. Deploy the title through the documented LAN-only procedure.
4. Capture the expected visual result and relevant logs.
5. Close the title, release the lock, and record firmware, loader, result, and
   artifact identity.
6. Commit the validation record separately from the implementation when the
   project workflow requires one.

Follow [Deployment](DEPLOYMENT.md) and the separate
[PS5 Homebrew Development Protocol](https://github.com/blackbearreloaded/ps5-homebrew-dev-protocol)
for console coordination, evidence collection, and milestone policy.

2026-10-07 | FW 12.70 / ShadowMount | 770fbb3 / 01.000.020 | PPSA88022: phone navigation, search, keyboard dismissal and playback Back passed; clean exit | evidence: results/phone-remote/

2026-10-07 | FW 12.70 / ShadowMount | 6e32fbf | 8888 unavailable; fallback 58145: user confirmed arrows, search, Clear, playback OK/Back; clean exit | evidence: results/phone-remote/port8888-*

2026-10-07 | FW 12.70 / ShadowMount | fb56c0a | phone icon and playback Favorite add/remove/persistence passed; clean exit | evidence: results/phone-remote/favorite-*

2026-10-07 | FW12.70 | 77f7237 | pass: QR, remembered/new pairing auto-close, volume sync, user-confirmed mute/audio; clean exit | results/phone-remote/autoclose-*

## Adding tests

- Add GoogleTest cases to C++ files under `tests/`; the `test-unit` recipe owns
  their host-only compilation.
- Add Python subprocess tests as `tests/test_*.py`; discovery is automatic.
- Preserve the GPL header on every test source.
- Run `make test`, `make lint`, and `make` before submitting a change.
- Reserve real-console claims for recorded hardware results.
