#!/usr/bin/env python3
# ProsperoTV - Edits that fit the copied sources and build script to this tree.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""usage: patch_tree.py <build tree>

Every edit names the exact text it replaces and fails when that text is not
found once, so a change in ProsperoTV or in the kit cannot be patched over
silently.
"""
import json
import os
import re
import sys
from pathlib import Path

tree = Path(sys.argv[1])


def swap(path, old, new, count=1):
    file = tree / path
    text = file.read_text(encoding="utf-8")
    if text.count(old) != count:
        sys.exit(f"{path}: expected {count} of {old[:60]!r}, found {text.count(old)}")
    file.write_text(text.replace(old, new), encoding="utf-8", newline="\n")


# ---- the keyboard: no SDL, and the controller is the menu's ----
swap("src/iptv_ime.c",
     '#include "iptv_input.h"\n\n#include <SDL2/SDL.h>\n#include <stddef.h>\n#include <stdint.h>\n',
     '#include <stddef.h>\n#include <stdint.h>\n#include <string.h>\n#include <time.h>\n\n'
     '/* The menu owns the controller: it says whether its confirm button is\n'
     ' * still down, so the press that asked for the keyboard is not typed. */\n'
     'extern bool iptv_ime_confirm_held(void);\n\n'
     'static void copy_text(char *output, const char *source, size_t capacity)\n'
     '{\n'
     '    size_t length = strlen(source);\n'
     '    if (capacity == 0)\n'
     '        return;\n'
     '    if (length >= capacity)\n'
     '        length = capacity - 1U;\n'
     '    memcpy(output, source, length);\n'
     '    output[length] = \'\\0\';\n'
     '}\n\n'
     'static uint32_t ticks_ms(void)\n'
     '{\n'
     '    struct timespec now = {0};\n'
     '    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)\n'
     '        return 0;\n'
     '    return (uint32_t)((uint64_t)now.tv_sec * UINT64_C(1000) +\n'
     '                      (uint64_t)now.tv_nsec / UINT64_C(1000000));\n'
     '}\n')
swap("src/iptv_ime.c",
     "    SDL_strlcpy(initial_text, value != NULL ? value : \"\", sizeof(initial_text));\n"
     "    SDL_strlcpy(requested_title, dialog_title != NULL ? dialog_title : \"\", sizeof(requested_title));\n"
     "    SDL_strlcpy(requested_placeholder, dialog_placeholder != NULL ? dialog_placeholder : \"\",\n"
     "                sizeof(requested_placeholder));\n",
     "    copy_text(initial_text, value != NULL ? value : \"\", sizeof(initial_text));\n"
     "    copy_text(requested_title, dialog_title != NULL ? dialog_title : \"\", sizeof(requested_title));\n"
     "    copy_text(requested_placeholder, dialog_placeholder != NULL ? dialog_placeholder : \"\",\n"
     "              sizeof(requested_placeholder));\n")
swap("src/iptv_ime.c", "    started_at = SDL_GetTicks();\n", "    started_at = ticks_ms();\n")
swap("src/iptv_ime.c",
     "    if (requested && !iptv_input_pressed(IPTV_INPUT_CROSS))\n",
     "    if (requested && !iptv_ime_confirm_held())\n")
swap("src/iptv_ime.c",
     "    if (status == 1 || (status == 0 && SDL_GetTicks() - started_at < 1000U))\n",
     "    if (status == 1 || (status == 0 && ticks_ms() - started_at < 1000U))\n")

# ---- the player's presenter: the menu's OpenGL runtime has already
#      initialised AGC in this process, and a second sceAgcInit says so
#      (0x8A6C0004 on hardware) without changing anything ----
swap("src/iptv_native_agc_present.c",
     "        result = sceAgcInit(&agc_state, 8);\n"
     "        if (result == 0)\n"
     "            agc_initialized = 1;\n",
     "        result = sceAgcInit(&agc_state, 8);\n"
     "        /* The menu's OpenGL runtime initialised AGC in this process first:\n"
     "         * the second call reports that and changes nothing. */\n"
     "        if ((uint32_t)result == UINT32_C(0x8A6C0004))\n"
     "            result = 0;\n"
     "        if (result == 0)\n"
     "            agc_initialized = 1;\n")

# ---- the player's messages ("IPTV: opening ...", errors, probes) went to the
#      console's notifications at the top left: the menu says what matters,
#      so they go to the log only ----
swap("src/iptv_player.cpp",
     "void Notify(const char *message)\n"
     "{\n"
     "    NotificationRequest request{};\n"
     "    if (message)\n"
     "    {\n"
     "        std::snprintf(request.message, sizeof(request.message), \"%s\", message);\n"
     "    }\n"
     "    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);\n"
     "}\n",
     "extern \"C\" int tv_diag_enabled(void);\n"
     "extern \"C\" void tv_diag_line(const char *line);\n"
     "\n"
     "void Notify(const char *message)\n"
     "{\n"
     "    // Into the log, not onto the screen: the menu tells the viewer what\n"
     "    // went wrong when it comes back.\n"
     "    if (message)\n"
     "    {\n"
     "        std::fprintf(stdout, \"[ProsperoTV][player] %s\\n\", message);\n"
     "        if (tv_diag_enabled())\n"
     "        {\n"
     "            char line[256];\n"
     "            std::snprintf(line, sizeof(line), \"player: %s\", message);\n"
     "            tv_diag_line(line);\n"
     "        }\n"
     "    }\n"
     "}\n")

# ---- the tuning screen (src/tv_tuning.cpp): the player's loading thread
#      shows the menu's picture with its bar moving, keeps it up while the
#      decoder opens, and lets it run out and fade before the first picture ----
swap("src/iptv_native_agc_present.h",
     "    int32_t iptv_native_agc_loading_start(void);\n"
     "    void iptv_native_agc_loading_stop(void);\n",
     "    int32_t iptv_native_agc_loading_start(void);\n"
     "    void iptv_native_agc_loading_stop(void);\n"
     "    /* The first picture is ready: the tuning screen finishes, fades out\n"
     "     * and gives the display up. Nothing happens without one. */\n"
     "    void iptv_native_agc_loading_finish(void);\n")
swap("src/iptv_native_agc_present.c",
     '#include "iptv_native_agc_present.h"\n',
     '#include "iptv_native_agc_present.h"\n#include "tv_tuning.h"\n')
swap("src/iptv_native_agc_present.c",
     "    if (!surface || loading.surface_bytes < LOADING_SURFACE_BYTES)\n"
     "        return -1;\n"
     "    memset(surface, 20, y_bytes);\n",
     "    if (!surface || loading.surface_bytes < LOADING_SURFACE_BYTES)\n"
     "        return -1;\n"
     "    if (tv_tuning_active())\n"
     "    {\n"
     "        /* The menu's tuning screen; 1 ends the thread once it has faded. */\n"
     "        if (!tv_tuning_compose(surface, loading.surface_bytes))\n"
     "            return 1;\n"
     "        flush_gpu_data(surface, LOADING_SURFACE_BYTES);\n"
     "        return iptv_native_agc_present_nv12(surface, loading.surface_bytes, LOADING_PITCH,\n"
     "                                            LOADING_SURFACE_HEIGHT, LOADING_PITCH,\n"
     "                                            LOADING_VISIBLE_HEIGHT, NULL);\n"
     "    }\n"
     "    memset(surface, 20, y_bytes);\n")
swap("src/iptv_native_agc_present.c",
     "        for (unsigned slice = 0; slice < 16u; ++slice)\n",
     "        /* The tuning screen moves every frame; presenting waits for the flip. */\n"
     "        for (unsigned slice = 0; !tv_tuning_active() && slice < 16u; ++slice)\n")
swap("src/iptv_native_agc_present.c",
     "    atomic_store_explicit(&present_cancelled, 0, memory_order_relaxed);\n"
     "    atomic_store_explicit(&loading.active, 1, memory_order_release);\n",
     "    atomic_store_explicit(&present_cancelled, 0, memory_order_relaxed);\n"
     "    tv_tuning_surface_changed();\n"
     "    atomic_store_explicit(&loading.active, 1, memory_order_release);\n")
swap("src/iptv_native_agc_present.c",
     "void iptv_native_agc_set_overlay_enabled(int enabled)\n",
     "void iptv_native_agc_loading_finish(void)\n"
     "{\n"
     "    void *thread_result = NULL;\n"
     "\n"
     "    if (!loading.thread || !tv_tuning_active())\n"
     "        return;\n"
     "    /* The loading thread runs the bar out and fades the screen, then ends. */\n"
     "    tv_tuning_finishing();\n"
     "    (void)scePthreadJoin(loading.thread, &thread_result);\n"
     "    loading.thread = NULL;\n"
     "    iptv_native_agc_loading_stop();\n"
     "    /* The video opens the display again at its own size. */\n"
     "    (void)teardown_presenter(1);\n"
     "}\n"
     "\n"
     "void iptv_native_agc_set_overlay_enabled(int enabled)\n")
swap("src/iptv_native_backend.c",
     "    started = monotonic_us();\n"
     "    state->telemetry.last_present_source = (uintptr_t)output->buffer;\n",
     "    /* The tuning screen leaves before the first picture is shown. */\n"
     "    if (!state->config.picture)\n"
     "        iptv_native_agc_loading_finish();\n"
     "    started = monotonic_us();\n"
     "    state->telemetry.last_present_source = (uintptr_t)output->buffer;\n")
swap("src/iptv_player.cpp",
     "    iptv_native_agc_loading_stop();\n"
     "    const int handoff = iptv_native_agc_present_shutdown();\n"
     "    if (handoff != 0)\n"
     "        return handoff;\n"
     "    const int result = iptv_native_backend_open(&adapter->backend, &config);\n"
     "    adapter->opened = result == 0;\n"
     "    return result;\n",
     "    // With the tuning screen up, it stays while the decoder opens and leaves\n"
     "    // just before the first picture (iptv_native_agc_loading_finish).\n"
     "    if (tv_tuning_active())\n"
     "    {\n"
     "        tv_tuning_stage(0.55f, 0.80f);\n"
     "    }\n"
     "    else\n"
     "    {\n"
     "        iptv_native_agc_loading_stop();\n"
     "        const int handoff = iptv_native_agc_present_shutdown();\n"
     "        if (handoff != 0)\n"
     "            return handoff;\n"
     "    }\n"
     "    const int result = iptv_native_backend_open(&adapter->backend, &config);\n"
     "    adapter->opened = result == 0;\n"
     "    if (result == 0 && tv_tuning_active())\n"
     "        tv_tuning_stage(0.82f, 0.96f);\n"
     "    return result;\n")
swap("src/iptv_player.cpp",
     '#include "iptv_player.h"\n',
     '#include "iptv_player.h"\n#include "tv_tuning.h"\n')

# ---- where the app's files are. The player and the stores name their files
#      by fixed sandbox paths; with filesystem access the files are in
#      /data/prosperotv and the app's folder is not /app0. Each constant
#      becomes a pointer that src/tv_storage.cpp settles before anything is
#      opened (src/tv_paths.h). ----
constant = re.compile(r'constexpr char (k\w+)\[\] =\s*"/download0/([^"/]+)";')
constants = 0
for folder in ("include", "src"):
    for file in sorted((tree / folder).glob("iptv_*")):
        text = file.read_text(encoding="utf-8")
        patched, count = constant.subn(r'const char *const \1 = tv_data_file("\2");', text)
        if count == 0:
            continue
        constants += count
        if file.suffix == ".h":
            # Before the header's first include.
            patched, placed = re.subn(r"^#include ", '#include "tv_paths.h"\n#include ', patched,
                                      count=1, flags=re.MULTILINE)
            if placed != 1:
                sys.exit(f"{file.name}: no place for tv_paths.h")
        file.write_text(patched, encoding="utf-8", newline="\n")
if constants != 9:
    sys.exit(f"expected 9 fixed data paths in the copied sources, found {constants}")
swap("src/iptv_player.cpp", '"/download0/iptv-attempt-receipt.txt"',
     'tv_data_file("iptv-attempt-receipt.txt")', 2)
swap("src/iptv_player.cpp", '#include "iptv_player.h"\n#include "tv_tuning.h"\n',
     '#include "iptv_player.h"\n#include "tv_paths.h"\n#include "tv_tuning.h"\n')
swap("src/iptv_native_agc_present.c",
     '    static const char path[] = "/app0/ui/fonts/lvgl-bitmap/Montserrat-32.tga";\n',
     '    const char *path = tv_app_file("ui/fonts/lvgl-bitmap/Montserrat-32.tga");\n')
swap("src/iptv_native_agc_present.c",
     '#include "iptv_native_agc_present.h"\n#include "tv_tuning.h"\n',
     '#include "iptv_native_agc_present.h"\n#include "tv_paths.h"\n#include "tv_tuning.h"\n')
# Nothing else may name a sandbox path: only the code that decides where the
# files are, and the update kit's defaults (replaced just below).
for file in sorted(tree.glob("src/**/*")) + sorted(tree.glob("include/*")):
    relative = file.relative_to(tree).as_posix()
    if not file.is_file() or file.suffix not in (".c", ".cpp", ".h", ".hpp") or \
            relative.startswith(("src/elevation/", "src/update_kit/", "src/kit/")) or \
            relative in ("src/tv_storage.cpp", "src/runtime/runtime_shims.c"):
        continue
    text = file.read_text(encoding="utf-8", errors="replace")
    if '"/download0' in text or '"/app0' in text:
        sys.exit(f"{relative} names a sandbox path; use tv_paths.h or tv_storage.hpp")

# ---- the decoders, woken before filesystem access. The system loads parts
#      of its video and audio decoders only when they are first used, and with
#      filesystem access the process no longer sees the sandbox those parts are
#      loaded from (on hardware: "load_prx failed due to 0x63", then
#      0x811D0111 from the first Videodec2 call). main() calls this first: it
#      loads both modules for the life of the process and asks each decoder the
#      questions that make it load the rest. Nothing is allocated or started. ----
swap("src/iptv_native_backend.c",
     "int32_t iptv_native_backend_init(iptv_native_backend_t *backend)\n{\n",
     "void iptv_native_backend_warm(int32_t results[6])\n"
     "{\n"
     "    /* One row of each codec: 1080p H.264, 1080p HEVC, 1080p VP9. */\n"
     "    static const unsigned rows[3] = {1u, 5u, 8u};\n"
     "    videodec2_compute_memory_t compute = {0};\n"
     "    unsigned index;\n"
     "\n"
     "    results[0] = sceSysmoduleLoadModule(VIDEO_MODULE_ID);\n"
     "    compute.size = sizeof(compute);\n"
     "    results[1] = sceVideodec2QueryComputeMemoryInfo(&compute);\n"
     "    for (index = 0; index < 3u; ++index)\n"
     "    {\n"
     "        const native_video_mode_t *mode = &video_modes[rows[index]];\n"
     "        videodec2_decoder_config_t config = {0};\n"
     "        videodec2_decoder_memory_t memory = {0};\n"
     "\n"
     "        config.size = sizeof(config);\n"
     "        config.resource_type = 1;\n"
     "        config.codec_type = mode->decoder_codec;\n"
     "        config.profile = mode->decoder_profile;\n"
     "        config.max_level = mode->max_level;\n"
     "        config.max_width = (int32_t)mode->decoder_max_width;\n"
     "        config.max_height = (int32_t)mode->decoder_max_height;\n"
     "        config.max_dpb_frames = 4;\n"
     "        config.pipeline_depth = 1u;\n"
     "        config.cpu_affinity = 0x3f;\n"
     "        config.cpu_priority = 700;\n"
     "        memory.size = sizeof(memory);\n"
     "        results[2u + index] = sceVideodec2QueryDecoderMemoryInfo(&config, &memory);\n"
     "    }\n"
     "    results[5] = sceSysmoduleLoadModule(AUDIO_MODULE_ID);\n"
     "    if (results[5] >= 0)\n"
     "    {\n"
     "        results[5] = sceAudiodecInitLibrary(AUDIODEC_AAC);\n"
     "        if (results[5] >= 0)\n"
     "            (void)sceAudiodecTermLibrary(AUDIODEC_AAC);\n"
     "    }\n"
     "}\n"
     "\n"
     "int32_t iptv_native_backend_init(iptv_native_backend_t *backend)\n{\n")

# ---- the self-update kit takes its three paths from the app (src/tv_update_paths.h) ----
swap("src/update_kit/self_update_ps5.c", '#include "self_update.h"\n',
     '#include "../tv_update_paths.h"\n\n#include "self_update.h"\n')

# ---- HTTP: every call the catalog and the player make to the system's
#      library goes to libcurl instead (src/tv_http.h says why). The calls
#      keep their arguments; only the names change. ----
http_names = {
    "sceSslInit": ("tv_http_tls_init", 2),
    "sceSslTerm": ("tv_http_tls_term", 2),
    "sceHttpInit": ("tv_http_init", 2),
    "sceHttpTerm": ("tv_http_term", 2),
    "sceHttpCreateTemplate": ("tv_http_create_template", 2),
    "sceHttpDeleteTemplate": ("tv_http_delete_template", 2),
    "sceHttpCreateConnectionWithURL": ("tv_http_create_connection", 3),
    "sceHttpDeleteConnection": ("tv_http_delete_connection", 2),
    "sceHttpCreateRequestWithURL": ("tv_http_create_request", 3),
    "sceHttpAbortRequest": ("tv_http_abort", 2),
    "sceHttpDeleteRequest": ("tv_http_delete_request", 2),
    "sceHttpAddRequestHeader": ("tv_http_add_header", 7),
    "sceHttpSetAutoRedirect": ("tv_http_set_redirect", 3),
    "sceHttpSetConnectTimeOut": ("tv_http_set_connect_timeout", 3),
    "sceHttpSetRecvTimeOut": ("tv_http_set_receive_timeout", 3),
    "sceHttpSetRecvBlockSize": ("tv_http_set_block_size", 2),
    "sceHttpSetSendTimeOut": ("tv_http_set_send_timeout", 3),
    "sceHttpSetResolveTimeOut": ("tv_http_set_resolve_timeout", 3),
    "sceHttpSendRequest": ("tv_http_send", 3),
    "sceHttpGetStatusCode": ("tv_http_status", 3),
    "sceHttpGetAllResponseHeaders": ("tv_http_headers", 3),
    "sceHttpReadData": ("tv_http_read", 6),
}
http_file = tree / "src/iptv_http.cpp"
http_text = http_file.read_text(encoding="utf-8")
for old, (new, expected) in http_names.items():
    http_text, found = re.subn(rf"\b{old}\b", new, http_text)
    if found != expected:
        sys.exit(f"src/iptv_http.cpp: expected {expected} of {old}, found {found}")
if re.search(r"\bsce(Http|Ssl)\w*", http_text):
    sys.exit("src/iptv_http.cpp still calls the system's HTTP library")
http_file.write_text(http_text, encoding="utf-8", newline="\n")

# ---- build.sh: the audio decoders, and the system modules the public SDK
#      has no import library for ----
swap("tools/build.sh",
     'bash "$root/tools/setup-native-dependencies.sh" >/dev/null\n\nparam=',
     'bash "$root/tools/setup-native-dependencies.sh" >/dev/null\n'
     'bash "$root/tools/setup-audio-dependencies.sh"\n\nparam=')
swap("tools/build.sh",
     '# The OpenGL runtime needs the process-lifetime heap in src/runtime/app_heap.c.\n',
     '# Ordinary ELF facades for system modules the public SDK has no import\n'
     '# library for. They exist only for the linker and the module writer.\n'
     'mkdir -p "$build/import-stubs"\n'
     'system_stub() {\n'
     '    local library=$1 soname=$2 source=$3 standard=$4\n'
     '    local object="$build/import-stubs/$library.o" output="$build/import-stubs/$library.so"\n'
     '    PS5_PAYLOAD_SDK="$sdk_root" PS5_CLANG="$target_compiler" USE_CCACHE=0 \\\n'
     '        sh "$root/tooling/prospero-clang18" "$standard" -O2 -fPIC \\\n'
     '        -ffunction-sections -fdata-sections -c "$source" -o "$object"\n'
     '    "$sdk_root/bin/prospero-lld" --shared -soname "$soname" -o "$output" "$object"\n'
     '    stub_paths+=("$output")\n'
     '    stub_options+=(--stub "$output")\n'
     '}\n'
     'system_stub libSceVideodec2 libSceVideodec2.prx \\\n'
     '    "$root/vendor/ps5/sdk/stubs/videodec2_link_stub.c" -std=c11\n'
     'system_stub libSceCommonDialog libSceCommonDialog.sprx \\\n'
     '    "$native/ps5_radio_import_stub_common_dialog.cpp" -std=c++20\n'
     'system_stub libSceAudiodec libSceAudiodec.sprx \\\n'
     '    "$native/ps5_radio_import_stub_audiodec.cpp" -std=c++20\n'
     '# The OpenGL runtime needs the process-lifetime heap in src/runtime/app_heap.c.\n')

# libcurl sets its sockets up with fcntl, which the console's C library
# refuses for a socket: every fcntl call goes through the wrapper in
# src/update_kit/console_curl.c.
swap("tools/build.sh", "wrap_options=()\n", "wrap_options=(--wrap=fcntl)\n")
# The two programs the app sends to the payload loader (filesystem access and
# the self-update) are built and packaged with the app.
swap("tools/build.sh",
     "printf '==> [zip] Archiving the application folder\\n'\n",
     'bash "$root/tools/package-extras.sh" "$title_id" "$app"\n\n'
     "printf '==> [zip] Archiving the application folder\\n'\n")

# ---- param.json. TV_CATEGORY picks the area the title shows in:
#   media (default)  as every release: Media, and no memory blocks
#   game             what every OpenGL title of the kit declares: Games, with
#                    the address-space and page-table blocks
param_path = tree / "sce_sys/param.json"
param = json.loads(param_path.read_text(encoding="utf-8"))
category = os.environ.get("TV_CATEGORY", "media")
if category == "game":
    param["applicationCategoryType"] = 0
    param["contentBadgeType"] = 1
    param["gameIntent"] = {"permittedIntents": [{"intentType": "launchActivity"}]}
    param["amm"] = {"multimapVaRangeInGib": 512, "pagetableMemorySizeInMib": 256,
                    "vaRangeInGib": 512}
    param["kernel"] = {"cpuPageTableSize": 268435456, "gpuPageTableSize": 268435456}
elif category != "media":
    sys.exit(f"TV_CATEGORY must be media or game, not {category!r}")

# ---- TV_TEST_TITLE=PPSA88nnn builds a disposable title for hardware checks:
#      it installs beside the released ProsperoTV, with storage of its own,
#      so a console test never touches the app the owner uses.
test_title = os.environ.get("TV_TEST_TITLE", "")
if test_title:
    if not re.fullmatch(r"PPSA88\d{3}", test_title):
        sys.exit("TV_TEST_TITLE must be PPSA88 followed by three digits")
    param["titleId"] = test_title
    param["conceptId"] = test_title[4:]
    param["contentId"] = f"UP9000-{test_title}_00-PROSPEROTVUITEST"
    for language in param["localizedParameters"].values():
        if isinstance(language, dict):
            language["titleName"] = "ProsperoTV UI test"
    # A test title can carry another version, to be the "newer release" of an
    # update test (tools/console-run.py, UPDATED_APP).
    test_version = os.environ.get("TV_TEST_VERSION", "")
    if test_version:
        if not re.fullmatch(r"\d{2}\.\d{3}\.\d{3}", test_version):
            sys.exit("TV_TEST_VERSION must look like 01.000.990")
        param["contentVersion"] = test_version
param_path.write_text(json.dumps(param, indent=2, sort_keys=True) + "\n", encoding="utf-8",
                      newline="\n")
# TV_DEBUG_TRACE=1 builds the app someone is sent to find out why their
# streams do not play: it writes logs/debug-trace.txt (see main.cpp).
debug_trace = os.environ.get("TV_DEBUG_TRACE", "") not in ("", "0")
# What the decoders ask of the system is wrapped in every build; the wrappers
# write only while the diagnostic log is on (ps5/diag/decoder_trace.c).
if True:
    import shutil

    shutil.copy(Path(__file__).resolve().parent / "diag/decoder_trace.c",
                tree / "src/runtime/decoder_trace.c")
    swap("tools/build.sh",
         '        wrap_options+=("--wrap=$symbol")\n    done\nfi\n',
         '        wrap_options+=("--wrap=$symbol")\n    done\nfi\n'
         'if [[ -f $root/src/runtime/decoder_trace.c ]]; then\n'
         '    for symbol in sceKernelAllocateDirectMemory sceKernelMapDirectMemory \\\n'
         '        sceKernelMapNamedFlexibleMemory sceVideodec2QueryComputeMemoryInfo \\\n'
         '        sceVideodec2AllocateComputeQueue sceVideodec2QueryDecoderMemoryInfo \\\n'
         '        sceVideodec2CreateDecoder sceSysmoduleLoadModule sceSysmoduleUnloadModule \\\n'
         '        sceAgcInit sceAgcCreateShader sceAgcLinkShaders sceAgcDriverSubmitDcb \\\n'
         '        sceAgcSuspendPoint sceVideoOutOpen sceVideoOutRegisterBuffers2 \\\n'
         '        sceAudiodecInitLibrary sceAudiodecCreateDecoder sceAudioOutInit sceAudioOutOpen; do\n'
         '        wrap_options+=("--wrap=$symbol")\n'
         '    done\n'
         'fi\n')
# The test title also reads scripted runs a PC leaves beside it.
(tree / "src/tv_build_options.h").write_text(
    "// ProsperoTV - What this build includes (written by ps5/patch_tree.py).\n#pragma once\n\n"
    f"#define TV_DEV_SCRIPTS {1 if test_title else 0}\n"
    f"#define TV_DEBUG_TRACE {1 if debug_trace else 0}\n"
    "// The title this build installs as: its folder and its Lapy helper carry it.\n"
    f"#define TV_TITLE_ID \"{param['titleId']}\"\n", encoding="utf-8", newline="\n")
print("tree patched, category", category, "title", param["titleId"])
