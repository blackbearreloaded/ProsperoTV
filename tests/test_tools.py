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
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
TITLE_ID = json.loads((ROOT / "sce_sys/param.json").read_text(encoding="utf-8"))["titleId"]


class ToolTests(unittest.TestCase):
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

        styles = (ROOT / "ui/styles/app.rcss").read_text(encoding="utf-8")
        self.assertIn("#source-region { left: 0px; width: 299px; }", styles)
        self.assertIn("#group-region { left: 315px; width: 931px; }", styles)


if __name__ == "__main__":
    unittest.main()
