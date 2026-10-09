#!/usr/bin/env python3
# ProsperoTV - Reproducible HDR color shader.
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
"""Assemble the Main10 color tail with LLVM (development tool, not a build dependency).

The existing pixel shader supplies R', G', B' in v0..2, and the unused
matrix constant s55 selects the mode:
0=unchanged, 1=PQ to SDR, 2=HLG to PQ, 3=HLG to SDR. Keep v0..8, s0..59
and the existing export contract; no shader metadata/resource changes.
BT.2100-2 Tables 4/5 define PQ and HLG. HLG uses a 1000-nit reference
display and its luminance OOTF, not independent channel powers.
"""
import argparse
from pathlib import Path
import struct
import subprocess
import tempfile


def shader():
    lines = []

    def emit(line):
        lines.append(line)

    def f(value):
        return hex(struct.unpack('<I', struct.pack('<f', value))[0])

    def mul(dst, value, src):
        emit(f'v_mul_f32_e32 v{dst}, {f(value)}, v{src}')

    def power(reg, exponent):
        emit(f'v_log_f32_e32 v{reg}, v{reg}')
        mul(reg, exponent, reg)
        emit(f'v_exp_f32_e32 v{reg}, v{reg}')

    emit('s_cmp_eq_u32 s55, 0')
    emit('s_cbranch_scc1 export_color')
    emit('s_cmp_eq_u32 s55, 1')
    emit('s_cbranch_scc1 pq_decode')
    for c in range(3):
        emit(f'v_mul_f32_e32 v3, v{c}, v{c}')
        mul(3, 1 / 3, 3)
        emit(f'v_add_f32_e32 v4, {f(-.55991073)}, v{c}')
        mul(4, 1.4426950408889634 / .17883277, 4)
        emit('v_exp_f32_e32 v4, v4')
        emit(f'v_add_f32_e32 v4, {f(.28466892)}, v4')
        mul(4, 1 / 12, 4)
        emit(f'v_cmp_gt_f32_e32 vcc_lo, 0.5, v{c}')
        emit(f'v_cndmask_b32_e32 v{c}, v4, v3, vcc_lo')
    mul(3, .2627, 0)
    mul(4, .6780, 1)
    emit('v_add_f32_e32 v3, v3, v4')
    mul(4, .0593, 2)
    emit('v_add_f32_e32 v3, v3, v4')
    power(3, .2)
    mul(3, 1000 / 203, 3)
    for c in range(3):
        emit(f'v_mul_f32_e32 v{c}, v3, v{c}')
    emit('s_cmp_eq_u32 s55, 2')
    emit('s_cbranch_scc0 sdr_encode')
    for c in range(3):
        mul(c, 203 / 10000, c)
        power(c, .1593017578125)
        mul(3, 18.8515625, c)
        emit(f'v_add_f32_e32 v3, {f(.8359375)}, v3')
        mul(4, 18.6875, c)
        emit('v_add_f32_e32 v4, 1.0, v4')
        emit('v_rcp_f32_e32 v4, v4')
        emit(f'v_mul_f32_e32 v{c}, v3, v4')
        power(c, 78.84375)
    emit('s_branch export_color')
    emit('pq_decode:')
    for c in range(3):
        power(c, 1 / 78.84375)
        mul(3, -18.6875, c)
        emit(f'v_add_f32_e32 v3, {f(18.8515625)}, v3')
        emit('v_rcp_f32_e32 v3, v3')
        emit(f'v_add_f32_e32 v{c}, {f(-.8359375)}, v{c}')
        emit(f'v_max_f32_e32 v{c}, 0, v{c}')
        emit(f'v_mul_f32_e32 v{c}, v3, v{c}')
        power(c, 1 / .1593017578125)
        mul(c, 10000 / 203, c)
    emit('sdr_encode:')
    matrix = ((1.660491, -.587641, -.072850), (-.124550, 1.132900, -.008350),
              (-.018151, -.100579, 1.118730))
    for row, coefficients in enumerate(matrix):
        mul(row + 3, coefficients[0], 0)
        for column in (1, 2):
            mul(6, coefficients[column], column)
            emit(f'v_add_f32_e32 v{row + 3}, v{row + 3}, v6')
    emit('v_max_f32_e32 v6, v3, v4')
    emit('v_max_f32_e32 v6, v6, v5')
    emit('v_max_f32_e32 v6, 0, v6')
    emit('v_add_f32_e32 v6, 1.0, v6')
    emit('v_rcp_f32_e32 v6, v6')
    for c in range(3):
        emit(f'v_mul_f32_e32 v{c}, v6, v{c + 3}')
        emit(f'v_max_f32_e32 v{c}, 0, v{c}')
    for c in range(3):
        mul(3, 12.92, c)
        emit(f'v_cmp_gt_f32_e32 vcc_lo, {f(.0031308)}, v{c}')
        power(c, 1 / 2.4)
        mul(c, 1.055, c)
        emit(f'v_add_f32_e32 v{c}, {f(-.055)}, v{c}')
        emit(f'v_cndmask_b32_e32 v{c}, v{c}, v3, vcc_lo')
    emit('export_color:')
    for c in range(3):
        emit(f'v_max_f32_e64 v{c}, 0, v{c} clamp')
    emit('v_cvt_pkrtz_f16_f32_e32 v1, v2, v1')
    emit('v_cvt_pkrtz_f16_f32_e64 v0, v0, 1.0')
    emit('s_mov_b64 exec, s[30:31]')
    emit('exp mrt0 v1, v1, v0, v0 done compr vm')
    emit('s_endpgm')
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--llvm-version', default='18')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    original = (root / 'assets/private/pixel.text.p010-passthrough.bin').read_bytes()
    branch = struct.pack('<I', 0xbf82017f)
    assert original.count(branch) == 1
    offset = original.index(branch)
    with tempfile.TemporaryDirectory() as temp:
        folder = Path(temp)
        (folder / 'color.s').write_text(shader(), encoding='utf-8')
        subprocess.run([f'llvm-mc-{args.llvm_version}', '--triple=amdgcn', '--mcpu=gfx1013',
                        '--filetype=obj', str(folder / 'color.s'), '-o', str(folder / 'color.o')],
                       check=True)
        subprocess.run([f'llvm-objcopy-{args.llvm_version}', '-O', 'binary', '--only-section=.text',
                        str(folder / 'color.o'), str(folder / 'color.bin')], check=True)
        data = (folder / 'color.bin').read_bytes()
    assert offset % 4 == 0 and len(data) % 4 == 0
    # Keep the original shader's trailer/metadata intact.
    code_end = original.index(struct.pack('<I', 0xbf810000)) + 4
    assert offset + len(data) <= code_end, (hex(offset), len(data), code_end)
    words = struct.unpack(f'<{len(data) // 4}I', data)
    header = ('/* ProsperoTV - Generated by tools/generate-hdr-shader.py; do not edit.\n'
              ' * Copyright (C) 2026 BlackBearReloaded\n'
              ' * SPDX-License-Identifier: GPL-3.0-or-later */\n'
              '#pragma once\n#include <stdint.h>\n'
              f'#define IPTV_HDR_SHADER_OFFSET {offset}u\n'
              'static const uint32_t iptv_hdr_shader[] = {\n')
    for i in range(0, len(words), 6):
        header += '    ' + ', '.join(f'0x{x:08x}u' for x in words[i:i+6]) + ',\n'
    header += '};\n'
    target = root / 'src/iptv_hdr_shader.h'
    target.write_text(header, encoding='utf-8')
    subprocess.run(['clang-format', '-i', str(target)], check=True)
    print(f'Color tail: {len(data)} bytes at {offset:#x}')


if __name__ == '__main__':
    main()
