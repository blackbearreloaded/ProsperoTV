# Deployment

The released app is the interface in [`opengl-ui/`](../opengl-ui/README.md),
since 01.000.020. It reaches a console as a complete title folder under
`/data/homebrew`: the folder from a release ZIP, or the one built here. This
repository has no deploy command for that app; the folder is copied over FTP.

The repository root still builds the interface of 01.000.015 and earlier, and
its `make deploy` uploads that build. It is described
[at the end](#the-earlier-interface-root-build) and is not how a release is
installed.

Nothing here configures the console, starts payloads, registers titles, or
creates a signed retail package.

## Requirements

Use a console you own with an already configured, compatible homebrew
environment and loader. Follow that loader's documentation for setup and
supported input formats. This repository does not configure the console.

Keep loader, mount, and FTP services on a trusted local network. Copying a
folder needs an already-running FTP service on the console (port `2121` in the
setups this project was validated on) and any FTP client.

## Install a release

Tagged GitHub Releases provide `PPSA99003.zip`, which holds the complete
`PPSA99003` title folder, and `SHA256SUMS`. A pull request's build is the same
ZIP under another artifact name: see
[Pull-request builds](PULL_REQUEST_BUILDS.md).

1. Check the download: `sha256sum -c SHA256SUMS`.
2. Fully close ProsperoTV if it is running.
3. Extract the ZIP and upload the contained `PPSA99003` folder to
   `/data/homebrew`, producing `/data/homebrew/PPSA99003/eboot.bin`.
4. Restart ShadowMountPlus or the PS5, wait for the title to be rediscovered,
   and launch it from the Media section of the home screen.

Do not upload the ZIP itself or only `eboot.bin`: the app also needs its
runtime module, fonts, sounds, artwork, metadata, and the two helper programs
beside `eboot.bin` (`lapy.elf` and `self-updater.elf`). Do not keep a folder
and an image with the same title ID in scan paths at the same time; coming
from an installed `PPSA99003.ffpfsc`, delete it first (see
[Updating ProsperoTV](../README.md#updating-prosperotv)).

The ZIP stores every entry with permissions 0777, because the console only
starts an app whose files are open to every user. A tool that keeps the stored
permissions while unpacking therefore still produces a folder that starts.

From 01.000.020 on, an installed folder updates itself when the app has
filesystem access; copying by hand is needed for the first install, for a
build of your own, and for an app without that access.

## Build the released app

Building requires Linux, WSL, or a Linux CI runner; the packages are listed in
the [README](../README.md#requirements) and in the `app` job of
[`.github/workflows/tooling.yml`](../.github/workflows/tooling.yml), which is
the build every release comes from. From the repository root:

```bash
opengl-ui/tools/run-tests.sh           # the interface and its logic, on the PC
opengl-ui/ps5/assemble.sh              # make the build tree beside the repository
make -C ../prosperotv-ui-build app     # compile, link, sign, assemble
```

`assemble.sh` takes the build tree's path as an optional argument; the default
is `../prosperotv-ui-build`. It fetches every dependency at its pinned version.
The outputs are:

```text
../prosperotv-ui-build/dist/PPSA99003/       complete title folder
../prosperotv-ui-build/dist/PPSA99003.zip    the same folder as a ZIP
```

The workflow runs `python3 tools/zip-open-modes.py <ZIP>` over that ZIP before
it uploads or publishes it, which rewrites the stored permissions to 0777. Do
the same to a ZIP you pass on.

Two variants of the build, set when the tree is assembled:

```bash
TV_TEST_TITLE=PPSA88021 opengl-ui/ps5/assemble.sh   # a disposable title beside the released app
TV_DEBUG_TRACE=1 opengl-ui/ps5/assemble.sh          # the app with its diagnostic log always on
```

The test title has its own folder under `/data/homebrew`, so trying a build
does not replace the installed app.

## Copy a built folder to the console

1. Fully close the app and any remaining crash dialog.
2. Upload the whole `dist/<TITLE_ID>/` folder to `/data/homebrew/<TITLE_ID>/`
   with an FTP client, replacing the files that are there. Finish with
   `eboot.bin` and `sce_sys/param.json`, so the title is complete only when
   everything else has landed.
3. Wait for the mount service to report the title ready (restart
   ShadowMountPlus or the PS5 if it does not pick the change up), then launch
   it by hand.
4. Fully close the app before copying again.

Files that a newer build no longer has are not removed by an upload; remove
the title folder first when an exact copy matters (see the next section).
Update `contentVersion` in `sce_sys/param.json` for a release-worthy change;
routine copies do not need a bump.

For scripted runs of the test title, `opengl-ui/tools/console-run.py
<console address> <app folder> <results> <script>` uploads the folder with
every file verified, launches the title, and collects its report and logs. It
needs the launch helper of the separate
[PS5 Homebrew Development Protocol](https://github.com/blackbearreloaded/ps5-homebrew-dev-protocol)
(`PS5_PROTOCOL`); its header lists the environment it reads, and the scripts
are in `opengl-ui/ps5/scripts/`. See
[`opengl-ui/README.md`](../opengl-ui/README.md).

## Remove the staged copy

Fully close the application, then remove the current `titleId` from the FTP
staging area:

```bash
make undeploy PS5_HOST=192.168.1.100
```

The command works on the title ID in the root `sce_sys/param.json`
(`PPSA99003`), whichever interface the installed folder holds. It recursively
removes only `/data/homebrew/<TITLE_ID>/`, and deletes an exact same-ID
`.ffpkg` image, a `.ffpfsc` image left by an older version, and
interrupted-upload temporary images. It never deletes
the `/data/homebrew` root or another title, and it does not touch the app's
data in `/data/prosperotv`. Preview the resolved targets without a network
request by adding `DEPLOY_DRY_RUN=1`; `FTP_PORT`, `PS5_FTP_USER`, and
`PS5_FTP_PASSWORD` are as in the table below.

This is deliberately named **undeploy**, not uninstall: FTP removal does not
unregister the title from the PS5 Shell database. A stale home-screen entry may
remain until the loader refreshes or dedicated, separately authorized cleanup
tooling unregisters it. The command fails if the FTP server cannot enumerate a
directory safely or if an active mount prevents removal.

## The earlier interface (root build)

The root `Makefile` builds the interface of 01.000.015 and earlier. It shares
the catalog, the stores, and the player with the released app and is kept for
their tests; it is no longer released. It has the **same title ID**, so
deploying it replaces an installed release with the earlier interface.

```bash
make                                   # dist/<TITLE_ID>/
make deploy PS5_HOST=192.168.1.100     # build that folder and upload it over FTP
```

`make deploy` (`tools/deploy.sh`) builds `dist/<TITLE_ID>/`, uploads each file
under a hidden `.upload` name and replaces the destination only after its
transfer completes, publishes `eboot.bin` and then `sce_sys/param.json` last,
and verifies that both are in the remote directory. It uploads only: it does
not launch the app, and it does not delete remote files the build no longer
has. Fully close the application before deploying and do not launch it until
the command finishes.

| Variable | Default | Purpose |
| --- | --- | --- |
| `PS5_HOST` | required | Console IPv4 address or hostname |
| `FTP_PORT` | `2121` | FTP service port |
| `DEPLOY_FORMAT` | `folder` | `folder` or `ffpkg` output |
| `PS5_FTP_USER` | `anonymous` | FTP username |
| `PS5_FTP_PASSWORD` | `codex` | FTP password |
| `DEPLOY_DRY_RUN` | `0` | Use `1` to build and print the target without networking |

Copy `.env.example` to the ignored `.env` file to keep `PS5_HOST`, `FTP_PORT`,
and `DEPLOY_FORMAT` between runs; command-line Make values still override it.
`make deploy PS5_HOST=192.0.2.1 DEPLOY_DRY_RUN=1` checks the local build and
the resolved destination without contacting a console.

The UFS2 image is a local option of this root build only: `make ffpkg`
writes `dist/<TITLE_ID>.ffpkg`, and `DEPLOY_FORMAT=ffpkg` uploads it (see
[Build output formats](FFPKG.md)). CI and releases carry the ZIP only, and an
app installed as an image cannot update itself.

## Smoke test

This is for the released app: the folder from a release ZIP
([Install a release](#install-a-release)), or the one built from
[`opengl-ui/`](../opengl-ui/README.md)
([Build the released app](#build-the-released-app)). The root `make` and
`make deploy` build the interface of 01.000.015 and earlier, which looks
different.

1. Launch ProsperoTV. The opening plays once (an old television switches on;
   any button ends it, and Reduce motion skips it), then the menu is there on
   Live TV.
2. On a first launch, keep the console online and leave the app open while it
   downloads and caches the iptv-org catalog. Later launches show the cached
   channels immediately. If a newer version is listed on homebrew.page, an
   update dialog opens over the menu; **Later** leaves everything as it is.
3. Open a channel with Cross. The tuning screen stays until the channel's
   first picture; closing playback returns to the same screen and channel. A
   channel that does not play says why, and is not by itself a failed start:
   public streams come and go.
4. Close the app through the home-screen interface.

`app.log` is in `/data/prosperotv/logs/` when the app was given filesystem
access, and in the title's `/download0/prosperotv/` when it was not.

If launch fails, or `make deploy` cannot reach the console, see
[Troubleshooting](TROUBLESHOOTING.md).
