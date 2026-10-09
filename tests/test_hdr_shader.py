# ProsperoTV - HDR shader reference checks.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the color shader's register program against independent transfer equations."""
import importlib.util
import math
from pathlib import Path
import random
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('hdr_shader', ROOT / 'tools/generate-hdr-shader.py')
GEN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(GEN)


def execute(rgb, mode):
    code = GEN.shader().splitlines()
    labels = {line[:-1]: i for i, line in enumerate(code) if line.endswith(':')}
    registers = dict(zip(('v0', 'v1', 'v2'), rgb))
    registers['s55'] = mode
    condition = vector_condition = False

    def value(token):
        if token in registers:
            return registers[token]
        if token.startswith('0x'):
            return struct.unpack('<f', struct.pack('<I', int(token, 16)))[0]
        return float(token)

    pc = 0
    while pc < len(code):
        line = code[pc]
        pc += 1
        if line.endswith(':'):
            continue
        op, args = line.split(' ', 1)
        args = args.split(', ')
        clamp = args[-1].endswith(' clamp')
        args[-1] = args[-1].removesuffix(' clamp')
        if op == 's_cmp_eq_u32':
            condition = value(args[0]) == value(args[1])
        elif op.startswith('s_cbranch_'):
            if condition == op.endswith('scc1'):
                pc = labels[args[0]]
        elif op == 's_branch':
            pc = labels[args[0]]
        elif op == 's_mov_b64':
            assert args == ['exec', 's[30:31]']
        elif op == 'v_cmp_gt_f32_e32':
            vector_condition = value(args[1]) > value(args[2])
        elif op.startswith('v_cvt_pkrtz'):
            registers[args[0]] = (value(args[1]), value(args[2]))
        elif op == 'v_mov_b32_e32':
            registers[args[0]] = value(args[1])
        elif op == 'exp':
            # Component order is part of the VideoOut contract, not just math.
            return [*registers['v1'], registers['v0'][0]]
        else:
            a = value(args[1])
            if op == 'v_cndmask_b32_e32':
                result = value(args[2]) if vector_condition else a
            elif op.startswith('v_mul_f32'):
                result = a * value(args[2])
            elif op.startswith('v_add_f32'):
                result = a + value(args[2])
            elif op.startswith('v_max_f32'):
                result = max(a, value(args[2]))
            elif op == 'v_rcp_f32_e32':
                result = 1 / a
            elif op == 'v_log_f32_e32':
                result = math.log2(a) if a else -math.inf
            elif op == 'v_exp_f32_e32':
                result = 2 ** a
            else:
                raise AssertionError(op)
            if clamp:
                result = min(1, max(0, result))
            registers[args[0]] = struct.unpack('<f', struct.pack('<f', result))[0]
    raise AssertionError('missing export')


def reference(rgb, mode):
    if mode in (0, 4):
        return rgb
    if mode == 1:
        def decode(x):
            p = x ** (32 / 2523)
            return 10000 * (max(p - 3424 / 4096, 0) / (2413 / 128 - 2392 / 128 * p)) ** (16384 / 2610)
        light = [decode(x) for x in rgb]
    else:
        scene = [x*x / 3 if x <= .5 else (math.exp((x-.55991073)/.17883277)+.28466892)/12 for x in rgb]
        gain = 1000 * sum(a*b for a, b in zip(scene, (.2627, .6780, .0593))) ** .2
        light = [x * gain for x in scene]
    if mode == 2:
        def encode(x):
            p = (x / 10000) ** (2610 / 16384)
            return ((3424 / 4096 + 2413 / 128 * p) / (1 + 2392 / 128 * p)) ** (2523 / 32)
        return [encode(x) for x in light]
    matrix = ((1.660491, -.587641, -.072850), (-.124550, 1.132900, -.008350),
              (-.018151, -.100579, 1.118730))
    linear = [sum(a*b for a, b in zip(row, light)) / 203 for row in matrix]
    divisor = 1 + max(0, *linear)
    linear = [max(0, x) / divisor for x in linear]
    return [12.92*x if x <= .0031308 else 1.055*x**(1/2.4)-.055 for x in linear]


class HdrShaderTest(unittest.TestCase):
    def test_reference_colors_and_modes(self):
        colors = [(0, 0, 0), (.5, .5, .5), (.75, .75, .75), (1, 1, 1),
                  (1, 0, 0), (0, 1, 0), (0, 0, 1), (.01, .02, .03)]
        rng = random.Random(2100)
        colors += [tuple(rng.random() for _ in range(3)) for _ in range(100)]
        for mode in range(5):
            for rgb in colors:
                with self.subTest(mode=mode, rgb=rgb):
                    actual, expected = execute(rgb, mode), reference(rgb, mode)
                    if mode not in (2, 4):
                        expected = expected[::-1] # SDR output is B8G8R8A8.
                    for a, b in zip(actual, expected):
                        self.assertAlmostEqual(a, b, delta=0.0001)
                    self.assertTrue(all(0 <= x <= 1 for x in actual))


if __name__ == '__main__':
    unittest.main()
