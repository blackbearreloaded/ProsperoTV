# Build output formats

Every application build creates and validates `dist/<TITLE_ID>/`.

Tagged GitHub Releases and every CI build attach a `.zip` containing the
complete title folder and its `SHA256SUMS`, and nothing else. The UFS2 image
below (`make ffpkg`) remains a local option for anyone who wants an image on
their own machine. The Make targets map to the same PowerShell
`-OutputFormat` selections:

| Make target / selection | Additional output | Packaging tool |
| --- | --- | --- |
| `make app` / `Folder` | None | None |
| `make ffpkg` / `Ffpkg` | `dist/<TITLE_ID>.ffpkg` | UFS2Tool |

```bash
make app
make ffpkg
```

`-Ffpkg` remains accepted as a compatibility alias for
`-OutputFormat Ffpkg` in the Windows PowerShell frontend.

## UFS2 FFPKG

The `.ffpkg` option creates and checks an uncompressed UFS2 filesystem image:

```text
ufs2tool makefs -S 4096 -b 20% -t ffs \
  -o version=2,bsize=32768,fsize=4096,minfree=0,softupdates=0,optimization=space \
  <title.ffpkg> <app-directory>
```

On first use, `tools/setup-packaging-dependencies.sh` or the equivalent
PowerShell bootstrapper (`tools/setup-ffpkg-tooling.ps1`) fetches
[SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) at commit
`b5307a60d5b4e3a68ba680e0e33cfadf05017c77`, builds its CLI with the .NET SDK
8 or newer, and caches it under ignored `.deps/UFS2Tool/`. The repository does
not distribute UFS2Tool source or binaries. The build reserves allocation
slack and verifies the resulting UFS2 superblock magic.

The same UFS2Tool-generated image was mounted through ShadowMountPlus and
launched successfully on PS5 system software 6.02 and 12.70.

Despite the similar names, `.ffpkg` here is a mountable filesystem image. This
project does not create a signed retail PKG/FPKG container.

An image from an older build is not deleted when the folder is built again.
Rebuild the image immediately before deploying it so an old one is not
mistaken for the current app.

The compressed `.ffpfsc` image of earlier versions is no longer built, and
`make ffpfsc` and `make packages` are gone with it. If an old
`<TITLE_ID>.ffpfsc` is still in `/data/homebrew`, delete it before installing
the folder: a folder and an image with the same title ID must not both be in
the loader's scan paths.
