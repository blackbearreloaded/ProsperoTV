#!/usr/bin/env python3
# ProsperoTV - Independent loudness metering regression checks.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Independent meter/behavior checks against FFmpeg, using synthetic PCM."""
import json
import pathlib
import re
import shutil
import subprocess
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
FF = shutil.which('ffmpeg')
PROBE = ROOT / 'build/tests/audio_normalize_probe'

def pcm(frequency, amplitude, seconds=12):
    source = f'aevalsrc={amplitude}*sin(2*PI*{frequency}*t)|{amplitude}*sin(2*PI*{frequency}*t):s=48000:d={seconds}'
    return subprocess.run([FF, '-v', 'error', '-f', 'lavfi', '-i', source, '-f', 's16le', '-'],
                          check=True, capture_output=True).stdout

def loudness(data):
    r = subprocess.run([FF, '-hide_banner', '-f', 's16le', '-ar', '48000', '-ac', '2', '-i', '-',
                        '-af', 'loudnorm=print_format=json', '-f', 'null', '-'],
                       input=data, capture_output=True, check=True)
    return json.loads(re.findall(rb'\{\s*"input_i".*?\}', r.stderr, re.S)[-1])

@unittest.skipUnless(FF and PROBE.exists(), 'Build the host probe and install FFmpeg first')
class AudioNormalizeFFmpegTest(unittest.TestCase):
    def test_k_weighted_meter_matches_independent_filter(self):
        for frequency in [100, 1000, 10000]:
            with self.subTest(frequency=frequency):
                data = pcm(frequency, .05)
                measured = float(loudness(data)['input_i'])
                result = subprocess.run([str(PROBE), '1'], input=data, check=True,
                                        capture_output=True)
                reported = float(re.search(rb'meter_lufs=([\d.-]+)', result.stderr)[1])
                self.assertAlmostEqual(reported, measured, delta=.15)

    def test_quiet_audio_reaches_target_and_loud_audio_is_reduced(self):
        for amplitude in [.01, .4]:
            with self.subTest(amplitude=amplitude):
                data = pcm(1000, amplitude)
                result = subprocess.run([str(PROBE), '1'], input=data, check=True,
                                        capture_output=True)
                # Last 3 seconds, after acquisition and gain ramp.
                measured = loudness(result.stdout[-3 * 48000 * 4:])
                self.assertAlmostEqual(float(measured['input_i']), -23, delta=.3)
                self.assertLessEqual(float(measured['input_tp']), -.8)

    def test_extremely_quiet_audio_respects_boost_cap(self):
        data = pcm(1000, .008)
        source_level = float(loudness(data)['input_i'])
        result = subprocess.run([str(PROBE), '1'], input=data, check=True, capture_output=True)
        measured = float(loudness(result.stdout[-3 * 48000 * 4:])['input_i'])
        self.assertAlmostEqual(measured - source_level, 18, delta=.15)
        self.assertLess(measured, -23)

    def test_bypass_preserves_pcm_exactly(self):
        data = pcm(1000, .4, 1)
        result = subprocess.run([str(PROBE), '0'], input=data, check=True, capture_output=True)
        self.assertEqual(result.stdout, data)

if __name__ == '__main__':
    unittest.main()
