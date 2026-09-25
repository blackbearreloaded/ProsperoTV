#!/usr/bin/env python3
# ps5-native-app-boilerplate - Host tooling regression tests.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Exercises identity initialization and deployment resolution without a console.

import json
import os
from pathlib import Path
import shutil
import shlex
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TITLE_ID = json.loads((ROOT / "sce_sys/param.json").read_text(encoding="utf-8"))["titleId"]


class ToolTests(unittest.TestCase):
    def test_audio_fallback_downmix_and_reset(self):
        flags = shlex.split(subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", "libavcodec", "libswresample", "libavutil"], text=True))
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "audio")
            subprocess.run(["clang", "-std=c11", "-O1", "-fsanitize=address,undefined", "-Iinclude",
                "src/iptv_audio_decode.c", "tests/test_audio_decode.c", *flags, "-o", executable], cwd=ROOT, check=True)
            for kind, fixture in ((15, "aac-stereo.aac"), (15, "aac-surround.bin"),
                                  (129, "ac3-surround.bin"), (135, "eac3-surround.bin"), (17, "aac-latm.bin")):
                with self.subTest(fixture=fixture):
                    subprocess.run([executable, str(kind), str(ROOT / "tests/fixtures" / fixture)], check=True, timeout=10)

    def test_mp2_decodes_mpeg1_stereo_and_mpeg2_mono(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "mp2")
            subprocess.run(["clang", "-std=c11", "-O1", "-fsanitize=address,undefined",
                            "-Iinclude", "tests/test_mp2_decode.c", "-lm", "-o", executable],
                           cwd=ROOT, check=True)
            subprocess.run([executable, str(ROOT / "tests/fixtures/mp2-stereo-48k.mp2"),
                            str(ROOT / "tests/fixtures/mp2-mono-24k.mp2")], check=True, timeout=10)

    def test_startup_probe_is_diagnostic_only_and_flushes_each_stage(self):
        source = (ROOT / "src/main.cpp").read_text()
        helper = source.split("void StartupProbe(", 1)[1].split("struct NotificationRequest", 1)[0]
        self.assertIn("#if IPTV_PROBE", helper)
        self.assertIn('reset ? "w" : "a"', helper)
        self.assertIn("std::fclose(file)", helper)
        self.assertIn('StartupProbe("main-entered", true);', source)
        self.assertIn('StartupProbe("catalog-ready");', source)
        with tempfile.TemporaryDirectory() as directory:
            first, second = Path(directory) / "first.txt", Path(directory) / "second.txt"
            helper = helper.replace("/data/ProsperoTV-startup-probe.txt", str(first))
            helper = helper.replace("/download0/iptv-startup-probe.txt", str(second))
            unit = Path(directory) / "startup.cpp"
            unit.write_text('#include <cstdio>\n#include <initializer_list>\n#define IPTV_PROBE 1\nvoid StartupProbe('
                            + helper + '\nint main() { StartupProbe("old", true); StartupProbe("main-entered", true); StartupProbe("catalog-ready"); }\n')
            executable = str(Path(directory) / "startup")
            subprocess.run(["clang++", "-std=c++20", str(unit), "-o", executable], check=True)
            subprocess.run([executable], check=True, timeout=5)
            for path in (first, second):
                self.assertEqual(path.read_text(), "curl-startup-v1 main-entered\ncurl-startup-v1 catalog-ready\n")

    def test_curl_probe_string_allocation_uses_app_heap(self):
        player = (ROOT / "src/iptv_player.cpp").read_text()
        helper = player.split("char *CurlProbeDuplicate", 1)[1].split("void RunCurlDownloadProbe", 1)[0]
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "duplicate.cpp"
            source.write_text('#include <cstdlib>\n#include <cstring>\n#include <cassert>\nchar *CurlProbeDuplicate' + helper + '''
int main() {
    const char *values[] = {"", "test string"};
    for (const char *value : values) {
        char *copy = CurlProbeDuplicate(value);
        assert(copy && copy != value && std::strcmp(copy, value) == 0);
        std::free(copy);
    }
}
''')
            executable = str(Path(directory) / "duplicate")
            subprocess.run(["clang++", "-std=c++20", str(source), "-o", executable], check=True)
            subprocess.run([executable], check=True, timeout=5)
        normalized = " ".join(player.split())
        self.assertIn("curl_global_init_mem(CURL_GLOBAL_DEFAULT, std::malloc", normalized)
        self.assertIn("std::free, std::realloc, CurlProbeDuplicate, std::calloc", normalized)

    def test_curl_probe_compatibility(self):
        player = (ROOT / "src/iptv_player.cpp").read_text()
        shim = player[player.index('extern "C" int getpwuid_r'):player.index("#endif")]
        for name in ("getpwuid_r", "recvmmsg", "sendmmsg", "if_nametoindex", "gmtime_r", "dladdr", "_setjmp", "_longjmp"):
            shim = shim.replace(name + "(", "probe_" + name + "(")
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "probe.cpp"
            source.write_text("#include <pwd.h>\n#include <errno.h>\n#include <fcntl.h>\n#include <unistd.h>\n#include <sys/socket.h>\n#include <cassert>\n#include <atomic>\n#include <time.h>\n#include <dlfcn.h>\n#include <setjmp.h>\n"
                + shim + '''
int main() {
    assert(probe_recvmmsg(-1, nullptr, 0, 0, nullptr) == -1 && errno == ENOSYS);
    assert(probe_sendmmsg(-1, nullptr, 0, 0) == -1 && errno == ENOSYS);
    assert(probe_if_nametoindex("unsupported") == 0 && errno == ENXIO);
    time_t epoch = 0;
    struct tm converted{};
    assert(probe_gmtime_r(&epoch, &converted) == &converted);
    assert(converted.tm_year == 70 && converted.tm_mon == 0 && converted.tm_mday == 1);
    assert(probe_gmtime_r(nullptr, &converted) == nullptr && errno == EINVAL);
    Dl_info info{};
    assert(probe_dladdr(&epoch, &info) == 0 && info.dli_fname == nullptr);
    jmp_buf context;
    const int resumed = probe__setjmp(context);
    if (resumed == 0)
        probe__longjmp(context, 7);
    assert(resumed == 7);
}
''')
            executable = str(Path(directory) / "probe")
            subprocess.run(["clang++", "-std=c++20", str(source), "-o", executable], check=True)
            subprocess.run([executable], check=True, timeout=5)

    def test_alternative_download_probe_is_bounded_and_keeps_tls_verification(self):
        player = (ROOT / "src/iptv_player.cpp").read_text()
        probe = player.split("void RunCurlDownloadProbe(", 1)[1].split("enum class FeedResult", 1)[0]
        self.assertIn('CURLOPT_PROTOCOLS_STR, "http,https"', probe)
        self.assertIn('CURLOPT_REDIR_PROTOCOLS_STR, "http,https"', probe)
        self.assertIn("CURLOPT_TIMEOUT_MS, 10000L", probe)
        self.assertIn("CURLOPT_MAXREDIRS, 5L", probe)
        self.assertIn("16u * 1024u * 1024u", probe)
        self.assertNotIn("CURLOPT_SSL_VERIFYPEER", probe)
        self.assertNotIn("CURLOPT_VERBOSE", probe)
        self.assertIn("curl_easy_cleanup(curl)", probe)
        self.assertIn("StopRequested()", probe)
        build = (ROOT / "tools/build-probe-curl.sh").read_text()
        self.assertIn("-DCURL_DISABLE_SOCKETPAIR=ON", build)
        self.assertIn("-DENABLE_THREADED_RESOLVER=ON", build)
        self.assertIn("nm -u", build)
        self.assertNotIn('extern "C" int pipe2', player)

    def test_download_probe_retains_prefix_and_is_bounded(self):
        player = (ROOT / "src/iptv_player.cpp").read_text()
        probe = player.split("        if (!download_probe_done)", 1)[1].split("        else\n#endif", 1)[0]
        self.assertIn("probe_capacity = 16u * 1024u * 1024u", probe)
        self.assertIn("MonotonicUsec() - started < UINT64_C(10000000)", probe)
        self.assertIn("!runner->StopRequested()", probe)
        self.assertIn("std::memcpy(prefix, read_buffer", probe)
        loop = probe.split("while (used < probe_capacity", 1)[1].split("gDirectDiagnostics.download_probe_us", 1)[0]
        self.assertNotIn("runner->Push", loop)
        self.assertNotIn("OpenStream", probe)
        self.assertIn("runner->Push(prefix, used)", probe)
        self.assertIn("delete[] prefix", probe)

    def test_network_batches_do_not_expand_demux_chunks(self):
        player = (ROOT / "src/iptv_player.cpp").read_text()
        self.assertIn("kNetworkReadBytes = 256u * 1024u", player)
        self.assertIn("std::uint8_t[kNetworkReadBytes]", player)
        self.assertNotIn("ReadStream(request, read_buffer, kReadBytes)", player)
        loop = player.split("    void ReadAheadLoop()", 1)[1].split("    int StopReadAhead", 1)[0]
        self.assertIn("chunk = kReadBytes", loop)
        self.assertNotIn("kNetworkReadBytes", loop)

    def test_native_buffering_and_timestamp_state(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = str(Path(directory) / "native-state")
            subprocess.run(["clang", "-std=gnu11", "-O2", "-ffunction-sections",
                            "-fdata-sections", "-DIPTV_NATIVE_BACKEND_STATE_TEST",
                            "-Iinclude", "-Isrc", "src/iptv_native_backend.c",
                            "-Xlinker", "--gc-sections", "-o", executable], cwd=ROOT, check=True)
            subprocess.run([executable], check=True, timeout=10)

    def test_http_receive_block_is_configured_on_template(self):
        http = (ROOT / "src/iptv_http.cpp").read_text()
        init = http.split("Status NetworkInit()", 1)[1].split("void NetworkShutdown()", 1)[0]
        self.assertIn("sceHttpSetRecvBlockSize(g_http_template, 64u * 1024u) < 0", init)

    def test_direct_probe_separates_network_and_queue_waits(self):
        player = (ROOT / "src/iptv_player.cpp").read_text()
        feed = player.split("FeedResult FeedRequest(", 1)[1].split("int SubmitWebmVideo", 1)[0]
        for operation, field in (("ReadStream(request", "read"), ("runner->Push(buffer", "push")):
            self.assertLess(feed.index(field + "_started = MonotonicUsec()"), feed.index(operation))
            self.assertLess(feed.index(operation), feed.index("diagnostics->" + field + "_total_us += elapsed"))
            self.assertIn("direct_" + field + "_total_us=%llu", player)
            self.assertIn("direct_" + field + "_max_us=%llu", player)

    def test_input_ring_empty_does_not_restart_startup_buffering(self):
        player = (ROOT / "src/iptv_player.cpp").read_text()
        loop = player.split("    void ReadAheadLoop()", 1)[1].split("    int StopReadAhead", 1)[0]
        # Buffering belongs to timestamped media queues, never the raw input ring.
        self.assertNotIn("primed", loop)
        self.assertNotIn("ReadAheadPrime", loop)

    def test_probe_preserves_attempt_before_cleanup(self):
        player = (ROOT / "src/iptv_player.cpp").read_text()
        close = player.split("    int Close()", 1)[1].split("    std::uint64_t PlayerCleanupCount", 1)[0]
        self.assertLess(close.index("StopReadAhead(false)"), close.index("SaveReceipt("))
        self.assertLess(close.index("SaveReceipt("), close.index("iptv_stream_cleanup("))
        self.assertIn("video_codec != IPTV_STREAM_VIDEO_UNKNOWN", close)
        self.assertIn('std::remove("/download0/iptv-attempt-receipt.txt")', player)
        self.assertIn("std::rename(temporary, receipt_path)", player)

    def run_init(self, param, **values):
        environment = os.environ.copy()
        environment.update(values)
        return subprocess.run(
            ["bash", str(ROOT / "tools/init-project.sh"), str(param)],
            cwd=ROOT,
            env=environment,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_init_coordinates_media_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            param.write_text(
                json.dumps(
                    {
                        "contentId": "UP9000-PPSA99999_00-HELLOWORLD000001",
                        "localizedParameters": {
                            "defaultLanguage": "en-US",
                            "en-US": {"titleName": "Old"},
                        },
                        "gameIntent": {"permittedIntents": [{"intentType": "launchActivity"}]},
                    }
                ),
                encoding="utf-8",
            )
            result = self.run_init(
                param,
                TITLE_ID="PPSA12345",
                APP_NAME="Moon Client",
                APP_CATEGORY="media",
                CONTENT_SUFFIX="MOONCLIENT000001",
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            configured = json.loads(param.read_text(encoding="utf-8"))
            self.assertEqual(configured["titleId"], "PPSA12345")
            self.assertEqual(configured["conceptId"], "12345")
            self.assertEqual(configured["contentId"], "UP9000-PPSA12345_00-MOONCLIENT000001")
            self.assertEqual(configured["localizedParameters"]["en-US"]["titleName"], "Moon Client")
            self.assertEqual(configured["applicationCategoryType"], 65536)
            self.assertEqual(configured["contentBadgeType"], 2)
            self.assertNotIn("gameIntent", configured)

    def test_init_rejects_invalid_title_without_rewriting(self):
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            original = '{"contentId":"UP9000-PPSA99999_00-HELLOWORLD000001"}\n'
            param.write_text(original, encoding="utf-8")
            result = self.run_init(param, TITLE_ID="PPSA12", APP_NAME="Broken")
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(param.read_text(encoding="utf-8"), original)

    def test_init_derives_game_suffix_and_preserves_mode(self):
        with tempfile.TemporaryDirectory() as directory:
            param = Path(directory) / "param.json"
            param.write_text(
                json.dumps(
                    {
                        "contentId": "UP9000-PPSA99999_00-HELLOWORLD000001",
                        "localizedParameters": {
                            "defaultLanguage": "en-US",
                            "en-US": {"titleName": "Old"},
                        },
                    }
                ),
                encoding="utf-8",
            )
            param.chmod(0o640)
            result = self.run_init(
                param, TITLE_ID="PPSA54321", APP_NAME="Native Sample", CONTENT_SUFFIX=""
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            configured = json.loads(param.read_text(encoding="utf-8"))
            self.assertEqual(configured["contentId"], "UP9000-PPSA54321_00-NATIVESAMPLE0000")
            self.assertEqual(configured["applicationCategoryType"], 0)
            self.assertEqual(configured["contentBadgeType"], 1)
            self.assertEqual(
                configured["gameIntent"]["permittedIntents"],
                [{"intentType": "launchActivity"}],
            )
            self.assertEqual(param.stat().st_mode & 0o777, 0o640)

    def test_undeploy_dry_run_resolves_only_current_title(self):
        environment = os.environ.copy()
        environment.update(PS5_HOST="192.0.2.1", DEPLOY_DRY_RUN="1")
        result = subprocess.run(
            ["bash", str(ROOT / "tools/deploy.sh"), "undeploy"],
            cwd=ROOT,
            env=environment,
            check=False,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"/data/homebrew/{TITLE_ID}/", result.stdout)
        self.assertIn(f"{TITLE_ID}.{{ffpkg,ffpfsc}}", result.stdout)
        self.assertIn("no network request was sent", result.stdout)

    def test_deploy_dry_run_uses_mocked_build_and_no_network(self):
        with tempfile.TemporaryDirectory() as directory:
            sandbox = Path(directory)
            (sandbox / "tools").mkdir()
            (sandbox / "sce_sys").mkdir()
            shutil.copy2(ROOT / "tools/deploy.sh", sandbox / "tools/deploy.sh")
            (sandbox / "sce_sys/param.json").write_text(
                '{"titleId":"PPSA12345"}\n', encoding="utf-8"
            )

            mock_bin = sandbox / "mock-bin"
            mock_bin.mkdir()
            mock_make = mock_bin / "make"
            mock_make.write_text(
                "#!/usr/bin/env bash\n"
                "mkdir -p \"$MOCK_ROOT/dist\"\n"
                "printf package > \"$MOCK_ROOT/dist/PPSA12345.ffpkg\"\n",
                encoding="utf-8",
            )
            mock_make.chmod(0o755)

            environment = os.environ.copy()
            environment.update(
                PS5_HOST="192.0.2.1",
                DEPLOY_DRY_RUN="1",
                DEPLOY_FORMAT="ffpkg",
                MOCK_ROOT=str(sandbox),
                PATH=f"{mock_bin}{os.pathsep}{environment['PATH']}",
            )
            result = subprocess.run(
                ["bash", str(sandbox / "tools/deploy.sh")],
                cwd=sandbox,
                env=environment,
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("/data/homebrew/PPSA12345.ffpkg", result.stdout)
            self.assertIn("no network request was sent", result.stdout)

    def test_boilerplate_runtime_and_title_layout_are_preserved(self):
        manifest = (ROOT / "runtime/libc.prx.sha256").read_text(encoding="utf-8").split()
        self.assertEqual(len(manifest), 2)
        self.assertEqual(manifest[1], "*libc.prx")

        build = (ROOT / "tools/build.sh").read_text(encoding="utf-8")
        for required in (
            'mkdir -p "$app/sce_sys" "$app/sce_module"',
            'self --sign --in "$build/eboot.elf" --out "$app/eboot.bin"',
            '(cd "$root/runtime" && sha256sum --check --strict libc.prx.sha256)',
            'runtime_modules=("$root/runtime/libc.prx")',
            'cp "$input" "$app/sce_module/$name"',
            'self --inspect --file "$app/eboot.bin"',
        ):
            self.assertIn(required, build)

    def test_release_is_media_and_topbar_uses_the_app_icon(self):
        param = json.loads((ROOT / "sce_sys/param.json").read_text(encoding="utf-8"))
        self.assertEqual(param["applicationCategoryType"], 65536)
        self.assertEqual(param["contentBadgeType"], 2)
        self.assertNotIn("gameIntent", param)

        rml = (ROOT / "ui/main.rml").read_text(encoding="utf-8")
        self.assertIn(
            '<img id="brand-mark" src="icons/prosperotv.tga" width="56" height="56"',
            rml,
        )
        styles = (ROOT / "ui/styles/app.rcss").read_text(encoding="utf-8")
        self.assertIn(
            "#brand-mark { position: absolute; left: 64px; top: 20px; width: 56px; "
            "height: 56px; }",
            styles,
        )
        icon = (ROOT / "ui/icons/prosperotv.tga").read_bytes()
        self.assertEqual(len(icon), 18 + 56 * 56 * 4 + 26)
        self.assertEqual(icon[:3], b"\x00\x00\x02")
        self.assertEqual(int.from_bytes(icon[12:14], "little"), 56)
        self.assertEqual(int.from_bytes(icon[14:16], "little"), 56)
        self.assertEqual(icon[16:18], b"\x20\x28")

    def test_native_video_handoff_closes_loading_presenter_before_decoder_open(self):
        player = (ROOT / "src/iptv_player.cpp").read_text(encoding="utf-8")
        start = player.index("int AdapterOpen(")
        end = player.index("int AdapterVideo(", start)
        adapter_open = player[start:end]
        stop = adapter_open.index("iptv_native_agc_loading_stop();")
        shutdown = adapter_open.index("iptv_native_agc_present_shutdown();")
        backend = adapter_open.index("iptv_native_backend_open(")
        self.assertLess(stop, shutdown)
        self.assertLess(shutdown, backend)

    def test_presenter_rejects_implicit_geometry_switch_and_uses_unique_markers(self):
        presenter = (ROOT / "src/iptv_native_agc_present.c").read_text(encoding="utf-8")
        self.assertIn("static uint64_t render_sequence;", presenter)
        self.assertIn("++render_sequence << 8", presenter)
        self.assertNotIn("status[3] == (uint64_t)render_marker", presenter)
        geometry = presenter.index("presenter.output_width != output.width")
        initialize = presenter.index("if (!presenter.ready)", geometry)
        self.assertIn("return -7;", presenter[geometry:initialize])

    def test_deferred_flip_waits_before_source_or_command_reuse(self):
        presenter = (ROOT / "src/iptv_native_agc_present.c").read_text(encoding="utf-8")
        backend = (ROOT / "src/iptv_native_backend.c").read_text(encoding="utf-8")
        self.assertIn("return defer_flip ? 0 : iptv_native_agc_present_finish_frame();", presenter)
        self.assertIn("return iptv_native_agc_present_nv12(surface,", presenter)
        self.assertIn("state->pending_present_source == frame_slot", backend)
        self.assertLess(
            backend.index("result = complete_pending_presentation(state);", backend.index("static int32_t present_video_output")),
            backend.index("result = iptv_native_agc_present_yuv_deferred("),
        )

    def test_4k_hevc_reserves_six_decoder_picture_buffers(self):
        backend = (ROOT / "src/iptv_native_backend.c").read_text(encoding="utf-8")
        self.assertIn(
            "state->config.codec == IPTV_NATIVE_CODEC_HEVC && "
            "state->mode->decoder_max_width >= 3840u) "
            "decoder_config.max_dpb_frames = 6;",
            " ".join(backend.split()),
        )

    def test_probe_records_first_hevc_headers_and_decoder_output_error(self):
        player = (ROOT / "src/iptv_player.cpp").read_text(encoding="utf-8")
        stream = (ROOT / "src/iptv_stream.cpp").read_text(encoding="utf-8")
        backend = (ROOT / "src/iptv_native_backend.c").read_text(encoding="utf-8")
        self.assertIn('hevc_parameter_mask(impl->video_es.data, bytes)', stream)
        self.assertIn('state->telemetry.decoder_output_error = output.error;', backend)
        self.assertIn('first_rap_hevc_parameter_mask=0x%x\\n', player)
        self.assertIn('"audio_stream_type=0x%02x\\naudio_pid=%u\\naudio_frames=%llu\\n"', player)

    def test_live_tv_back_and_source_layout_are_kept_simple(self):
        app = (ROOT / "src/iptv_app.cpp").read_text(encoding="utf-8")
        circle = app.index(
            "if (event.key == IptvInputKey::Circle)\n"
            "    {\n"
            "        if (error_retries_playback_)"
        )
        l1 = app.index("if (event.key == IptvInputKey::L1", circle)
        circle_handler = app[circle:l1]
        self.assertIn("page_offset_ = 0;", circle_handler)
        self.assertIn("focus_target_ = FocusTarget::Group;", circle_handler)
        self.assertIn("focus_slot_ = selected_group_;", circle_handler)
        self.assertNotIn("FocusTarget::LiveSource", app)

        rml = (ROOT / "ui/main.rml").read_text(encoding="utf-8")
        self.assertIn('<div id="source-slot-0" class="rail-item selected">', rml)
        self.assertIn('<div id="source-slot-1" class="rail-item hidden">', rml)
        self.assertIn('<div id="source-slot-2" class="rail-item hidden">', rml)
        self.assertIn('id="source-management-name-2">Add an Xtream Codes account', rml)
        self.assertIn('id="source-management-action-1" class="source-action"', rml)
        self.assertIn('id="source-management-action-2" class="source-action"', rml)
        self.assertIn('id="triangle-hint-label">Search', rml)
        self.assertIn('screen_ == Screen::Sources ? "Add / Edit source" : "Search"', app)

        styles = (ROOT / "ui/styles/app.rcss").read_text(encoding="utf-8")
        self.assertIn("#source-region { left: 0px; width: 299px; }", styles)
        self.assertIn("#group-region { left: 315px; width: 931px; }", styles)

    def test_xtream_password_uses_masked_native_ime(self):
        ime = (ROOT / "src/iptv_ime.c").read_text(encoding="utf-8")
        app = (ROOT / "src/iptv_app.cpp").read_text(encoding="utf-8")
        self.assertIn("#define SCE_IME_OPTION_PASSWORD UINT32_C(0x00000004)", ime)
        self.assertIn(".option = requested_option", ime)
        self.assertIn('iptv_ime_request_password("Xtream password"', app)
        self.assertIn("SCE_IME_ENTER_LABEL_SEARCH", ime)
        self.assertIn(".enter_label = requested_enter_label", ime)
        self.assertNotIn(".enter_label = 2", ime)


if __name__ == "__main__":
    unittest.main()
