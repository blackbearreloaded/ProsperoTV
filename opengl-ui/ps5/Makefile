# ProsperoTV - Build entry points of the assembled OpenGL build tree.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# ps5/assemble.sh puts this file at the root of the build tree. The recipe is
# the UI kit's (ps5-opengl, the wrapped heap, ninja); the sources are
# ProsperoTV's player and the new interface.

SHELL := /bin/bash
.DEFAULT_GOAL := app

IPTV_AUTOTEST ?= 0
IPTV_REMOTE_CAPTURE ?= 0
# Empty selects the pinned ps5-opengl release (tools/fetch-opengl-sdk.sh).
PS5_OPENGL_PREFIX ?=
OPENGL_SDK := .deps/ps5-opengl/current
FFMPEG := .deps/ffmpeg-audio/root

APP_DEFINITIONS := GL_GLEXT_PROTOTYPES=1 IPTV_AUTOTEST_ENABLED=$(IPTV_AUTOTEST) IPTV_PROBE=0
APP_DEFINITIONS += IPTV_REMOTE_CAPTURE=$(IPTV_REMOTE_CAPTURE)
APP_INCLUDE_PATHS := src src/kit include vendor/qrcodegen $(OPENGL_SDK)/include $(FFMPEG)/include
# The OpenGL link group brings the C++ runtime (libc++, libc++abi, libunwind).
APP_STATIC_ARCHIVES := $(FFMPEG)/lib/libavcodec.a $(FFMPEG)/lib/libswresample.a \
	$(FFMPEG)/lib/libavutil.a .deps/ps5-opengl/libps5opengl-group.a
APP_IMPORT_STUBS := $(OPENGL_SDK)/lib/libSceAgc.so $(OPENGL_SDK)/lib/libSceAgcDriver.so
APP_RUNTIME_MODULES ?=
# libcurl with OpenSSL: every request the app makes, and the update kit.
PACBREW_PACKAGES ?= libcurl
PACBREW_INCLUDE_PATHS := include
PACBREW_STATIC_ARCHIVES := lib/libsqlite3.a
BUILD_JOBS ?= $(shell nproc 2>/dev/null || echo 2)
USE_CCACHE ?= 1
export BUILD_JOBS USE_CCACHE PS5_OPENGL_PREFIX
export APP_DEFINITIONS APP_INCLUDE_PATHS APP_STATIC_ARCHIVES APP_IMPORT_STUBS APP_RUNTIME_MODULES
export PACBREW_PACKAGES PACBREW_INCLUDE_PATHS PACBREW_STATIC_ARCHIVES

RUNTIME := runtime/libc.prx

.PHONY: all app deps opengl audio libc clean

all: app

libc:
	@bash tools/rebuild-libc.sh

$(RUNTIME):
	@printf '%s\n' '==> [libc] Generating the runtime'
	@bash tools/rebuild-libc.sh

opengl:
	@printf '%s\n' '==> [opengl] Preparing the ps5-opengl SDK link group'
	@bash tools/prepare-opengl.sh

audio:
	@printf '%s\n' '==> [audio] Preparing the pinned audio decoders'
	@bash tools/setup-native-dependencies.sh >/dev/null
	@bash tools/setup-audio-dependencies.sh

deps: opengl audio

app: $(RUNTIME) opengl audio
	@printf '%s\n' '==> [app] Compiling, linking, signing, and assembling the app folder'
	@bash tools/build.sh Folder

clean:
	@rm -rf -- build dist
