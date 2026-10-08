#!/usr/bin/env bash
# ProsperoTV - Assemble the private console build tree.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# usage: ps5/assemble.sh [build tree]      (default: ../../prosperotv-ui-build, beside the repository)
#
# KIT the ps5-homebrew-ui checkout (default: fetched at its pinned commit by tools/fetch-kit.sh)
# TV  the ProsperoTV checkout      (default: the repository this folder is in)
# TV_CATEGORY=media|game           which area of the home screen (see patch_tree.py)
#
# Puts together a tree `make` builds into dist/<TITLE_ID>: the kit's build
# recipe for OpenGL titles, ProsperoTV's player and catalog sources, the kit's
# renderer and components, and the new interface with its console entry point.
# ProsperoTV's RmlUi interface, SDL and FreeType are left out. The result is a
# local folder with no remote: nothing here publishes anything.

set -euo pipefail
proto=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
kit=$(bash "$proto/tools/fetch-kit.sh")
tv=$(cd -- "${TV:-$proto/..}" && pwd)
out=${1:-"$proto/../../prosperotv-ui-build"}
mkdir -p "$out"
out=$(cd -- "$out" && pwd)
[[ $out != "$tv" && $out != "$kit" && $out != "$proto" ]] || {
    echo "the build tree must be a folder of its own" >&2
    exit 2
}
[[ ! -e $out/.git ]] || { echo "the build tree must not be a repository" >&2; exit 2; }

# Everything but the caches and the build output is made again.
for stale in src include assets sce_sys vendor tools tooling runtime update_helper third_party \
    Makefile; do
    rm -rf -- "${out:?}/$stale"
done
mkdir -p "$out"/{src,include,tools,vendor/ps5/sdk/stubs,assets/fonts,assets/audio/sfx}

# ---- the build recipe: the kit's, which every OpenGL title of ours uses ----
cp -a "$kit/tooling" "$out/tooling"
cp -a "$kit/runtime" "$out/runtime"
for tool in build.sh build-host-tools.sh ninja-build.sh prepare-opengl.sh fetch-opengl-sdk.sh \
    setup-native-dependencies.sh setup-pacbrew-dependencies.sh setup-packaging-dependencies.sh \
    rebuild-libc.sh validate-assets.sh; do
    cp "$kit/tools/$tool" "$out/tools/$tool"
done
cp "$proto/ps5/Makefile" "$out/Makefile"
# The two programs the app sends to the payload loader: upstream Lapy's
# helper (fetched and built at its pinned commit) and the self-update helper.
cp "$proto"/tools/{build-lapy-helper.py,validate-loader-elf.py,package-extras.sh} "$out/tools/"
cp -a "$proto/ps5/update_helper" "$out/update_helper"
cp -a "$proto/ps5/third_party" "$out/third_party"

# ---- ProsperoTV: the player, the catalog, the stores, the keyboard ----
for source in "$tv"/src/iptv_*.c "$tv"/src/iptv_*.cpp "$tv"/src/iptv_*.h "$tv/src/sqlite_compat.cpp"; do
    [[ ${source##*/} == iptv_app.cpp ]] || cp "$source" "$out/src/"
done
for header in "$tv"/include/*.h; do
    [[ ${header##*/} == iptv_app.h ]] || cp "$header" "$out/include/"
done
cp -a "$tv/vendor/minimp3" "$out/vendor/minimp3"
cp -a "$tv/vendor/qrcodegen" "$out/vendor/qrcodegen"
cp "$tv/vendor/ps5/sdk/stubs/videodec2_link_stub.c" "$out/vendor/ps5/sdk/stubs/"
cp "$tv"/tooling/native/ps5_radio_import_stub_{audiodec,common_dialog}.cpp "$out/tooling/native/"
cp "$tv/tools/setup-audio-dependencies.sh" "$out/tools/"
cp "$tv/tools/setup-guide-dependencies.sh" "$out/tools/"
cp -a "$tv/assets/." "$out/assets/"
cp -a "$tv/sce_sys" "$out/sce_sys"
# The home-screen artwork of this interface replaces the released one, and
# the title has no home-screen music: media apps on the console play none.
cp "$proto"/ps5/sce_sys/icon0.png "$proto"/ps5/sce_sys/pic0.dds "$proto"/ps5/sce_sys/pic1.dds "$out/sce_sys/"
rm -f -- "$out/sce_sys/snd0.at9"

# ---- the kit: renderer, components, input, sounds, the console's display ----
while IFS= read -r relative; do
    relative=${relative%$'\r'}
    [[ -n $relative ]] || continue
    mkdir -p "$out/src/kit/$(dirname "$relative")"
    cp "$kit/src/$relative" "$out/src/kit/$relative"
done < "$proto/ps5/kit-files.txt"

# ---- the interface and the console entry point ----
mkdir -p "$out/src/tv"
cp "$proto"/src/tv/* "$out/src/tv/"
cp -a "$proto/ps5/src/." "$out/src/"
# The kit's faces, baked with the alphabets channel names need.
fonts=$(bash "$proto/tools/bake-fonts.sh" 2>/dev/null)
cp "$fonts"/*.huifont "$fonts"/*-LICENSE.txt "$out/assets/fonts/"
cp -a "$kit/assets/audio/sfx/glass" "$out/assets/audio/sfx/glass"

# ---- caches: what another tree on this machine already fetched ----
mkdir -p "$out/.deps"
seed() {
    local name=$1
    shift
    [[ ! -e $out/.deps/$name ]] || return 0
    local from
    for from in "$@"; do
        if [[ -d $from/.deps/$name ]]; then
            cp -a "$from/.deps/$name" "$out/.deps/$name"
            return 0
        fi
    done
}
seed native "$kit"
seed pacbrew "$proto/../../prosperoradio-ui-build" "$tv"
seed ffmpeg-audio "$tv"
seed expat "$tv"
seed lapy "$proto/../../ps5-native-app-boilerplate"
if [[ ! -e $out/.deps/ps5-opengl && -d $kit/.deps/ps5-opengl ]]; then
    mkdir -p "$out/.deps/ps5-opengl"
    for release in "$kit"/.deps/ps5-opengl/ps5-opengl-sdk-*; do
        [[ -d $release ]] && cp -a "$release" "$out/.deps/ps5-opengl/"
    done
fi

python3 "$proto/ps5/patch_tree.py" "$out"
printf 'Build tree: %s\n' "$out"
