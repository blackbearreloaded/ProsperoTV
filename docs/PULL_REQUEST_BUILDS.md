# Pull-request builds

Every pull request is built in full by the [Build workflow](../.github/workflows/tooling.yml):
lint, host tests, the runtime reproduction, and the packaging steps. The result is kept for
14 days under a name that says which pull request and which commit it came from.

The workflow builds the app at the root of the repository (`src/`, through `make ffpfsc`).
The released interface is built from `opengl-ui/` by the same run's `app` job and is
**not** what this artifact contains: that one is uploaded as
`ProsperoTV-app-PR<number>-<commit>` and holds `PPSA99003.zip`, the folder to try on a
console.

## What a pull request produces

| | Pull request | Push to `main`, tag, manual run |
| --- | --- | --- |
| Artifact name | `ProsperoTV-PR<number>-<commit>` | `prospero-tv-<commit>-release` |
| `<commit>` | First seven characters of the pull request's own head commit | The full commit that was built |
| Label inside the app folder | `PR <number>, <commit>` | None |
| `contentVersion` | Unchanged | Unchanged |

Two details are deliberate:

- **The commit is the pull request's head**, not `github.sha`. For a pull request,
  `github.sha` is a temporary merge commit that appears nowhere on the pull request's page,
  so an artifact named after it cannot be matched to what is being reviewed.
- **The version is not touched.** A pull request's build reports the same `contentVersion`
  as the release it is based on, so anything that compares versions behaves exactly as it
  will after the merge. The label is a separate file.

The first part of the pull-request name is the repository's name, so a fork's own pull
requests are named after the fork.

## Getting the build

1. Open the pull request, then **Checks** and the **Build** run (or the run's page under
   **Actions**).
2. Download the artifact named `ProsperoTV-PR<number>-<commit>` from the run's
   **Artifacts** list. GitHub requires a signed-in account for this.
3. Unpack it: it holds `PPSA99003.zip` (the app folder), `PPSA99003.ffpfsc` (its image) and
   `SHA256SUMS`. Check the files with `sha256sum -c SHA256SUMS`; installing is described in
   [Deployment](DEPLOYMENT.md).

A first-time contributor's pull request does not build until a maintainer approves the
workflow run. That is GitHub's default for public repositories and is worth keeping: the
build runs the pull request's code.

## The label inside the app folder

`tools/build.sh` writes the environment variable `BUILD_LABEL` as one line to
`build-label.txt` at the root of the app folder, next to `eboot.bin`
(`/app0/build-label.txt` on the console). The workflow sets it for pull requests only. A
build without it writes no file, so a build of `main` or of a tag never carries one.

`BUILD_LABEL` must be 1 to 40 characters from letters, digits, spaces and `, . _ # -`. The
build refuses anything else before compiling, so the text is safe to show as it is.

The app does not show the label yet: today it is a file to read in the ZIP or on the
console. An app screen that shows the version can read the same file and show nothing when
it is missing. Do not put the label into `param.json` or compare it with anything: it is
for people.

The same works on your PC, for a build you want to tell apart:

```bash
BUILD_LABEL="pacing test 2" make
```

## Keeping it safe

Pull-request runs have a read-only token and no secrets, including for forks. Do not move
this build to `pull_request_target` to post links or comments: that event runs with write
access and secrets, and building a contributor's code under it hands both to that code.
