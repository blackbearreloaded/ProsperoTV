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
    text = text.replace(before, after, 1)

# Keep the existing SEI parser and publish standard FFmpeg side data, so the
# application never depends on FFmpeg's private structure layout.
header = '#include "libavutil/mastering_display_metadata.h"'
if header not in text:
    text = text.replace('#include "sei.h"', '#include "sei.h"\n' + header, 1)
marker = "    /* ProsperoTV: preserve HDR static metadata on the parsed picture. */"
if marker not in text:
    metadata = """
    /* ProsperoTV: preserve HDR static metadata on the parsed picture. */
    if (first_slice_in_pic_flag && IS_IRAP_NAL(nal))
        av_packet_side_data_free(&avctx->coded_side_data, &avctx->nb_coded_side_data);
    if (first_slice_in_pic_flag && sei->common.mastering_display.present) {
        AVPacketSideData *sd = av_packet_side_data_new(&avctx->coded_side_data,
            &avctx->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
            sizeof(AVMasteringDisplayMetadata), 0);
        if (!sd)
            return AVERROR(ENOMEM);
        AVMasteringDisplayMetadata *m = (void *)sd->data;
        memset(m, 0, sizeof(*m));
        const int order[3] = {2, 0, 1};
        for (int c = 0; c < 3; ++c)
            for (int xy = 0; xy < 2; ++xy)
                m->display_primaries[c][xy] = (AVRational){
                    sei->common.mastering_display.display_primaries[order[c]][xy], 50000};
        for (int xy = 0; xy < 2; ++xy)
            m->white_point[xy] = (AVRational){sei->common.mastering_display.white_point[xy], 50000};
        m->min_luminance = (AVRational){sei->common.mastering_display.min_luminance, 10000};
        m->max_luminance = (AVRational){sei->common.mastering_display.max_luminance, 10000};
        m->has_primaries = m->has_luminance = 1;
    }
    if (first_slice_in_pic_flag && sei->common.content_light.present) {
        AVPacketSideData *sd = av_packet_side_data_new(&avctx->coded_side_data,
            &avctx->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL,
            sizeof(AVContentLightMetadata), 0);
        if (!sd)
            return AVERROR(ENOMEM);
        AVContentLightMetadata *m = (void *)sd->data;
        m->MaxCLL = sei->common.content_light.max_content_light_level;
        m->MaxFALL = sei->common.content_light.max_pic_average_light_level;
    }
"""
    if text.count(after) != 1:
        raise SystemExit("Pinned HEVC VUI patch is missing")
    text = text.replace(after, after + metadata, 1)
path.write_text(text)
