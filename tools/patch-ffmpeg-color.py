#!/usr/bin/env python3
"""ProsperoTV: export HEVC parser VUI through public AVCodecContext fields.

FFmpeg 8.0.1 already parses these values but only exports them when decoding.
Our hardware decoder needs the same metadata without software picture decoding.
"""
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import sys

path = Path(sys.argv[1]) / "libavcodec/hevc/parser.c"
text = path.read_text()
before = "    avctx->level    = sps->ptl.general_ptl.level_idc;"
after = before + """
    avctx->color_primaries = sps->vui.common.colour_primaries;
    avctx->color_trc       = sps->vui.common.transfer_characteristics;
    avctx->colorspace     = sps->vui.common.matrix_coeffs;
    avctx->color_range    = sps->vui.common.video_full_range_flag ?
                            AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;"""
if text.count(after) != 1:
    if text.count(before) != 1:
        raise SystemExit("Pinned FFmpeg HEVC parser differs from the expected version")
    path.write_text(text.replace(before, after, 1))
