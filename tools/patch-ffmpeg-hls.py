#!/usr/bin/env python3
"""ProsperoTV: fix WebVTT buffer ownership in the pinned FFmpeg 8.0.1 HLS reader.

Its dummy subtitle header allocates an unused read buffer and frees the format
without its demuxer close callback. Each subsequent segment replaces another
custom AVIO buffer without freeing it. Our multi-segment subtitle fixture runs
under LeakSanitizer to cover both startup and segment turnover.
"""
# Copyright (C) 2026 BlackBearReloaded
# SPDX-License-Identifier: GPL-3.0-or-later
from pathlib import Path
import sys

path = Path(sys.argv[1]) / "libavformat/hls.c"
text = path.read_text()
changes = [
    (
        '''        if (pls->is_subtitle)
            ffio_init_context(&pls->pb, (unsigned char*)av_strdup("WEBVTT\\n"), (int)strlen("WEBVTT\\n"), 0, pls,
                                       NULL, NULL, NULL);
        else''',
        '''        if (pls->is_subtitle) {
            memcpy(pls->read_buffer, "WEBVTT\\n", 7);
            ffio_init_context(&pls->pb, pls->read_buffer, 7, 0, pls,
                                       NULL, NULL, NULL);
        } else''',
    ),
    (
        '''        if (pls->is_subtitle) {
            avformat_free_context(pls->ctx);
            pls->ctx = NULL;
            pls->needed = 0;''',
        '''        if (pls->is_subtitle) {
            avformat_close_input(&pls->ctx);
            av_freep(&pls->pb.pub.buffer);
            pls->read_buffer = NULL;
            pls->needed = 0;''',
    ),
    (
        '''    pls->read_buffer = av_malloc(INITIAL_BUFFER_SIZE);
    if (!pls->read_buffer) {''',
        '''    av_freep(&pls->pb.pub.buffer);
    pls->read_buffer = av_malloc(INITIAL_BUFFER_SIZE);
    if (!pls->read_buffer) {''',
    ),
]
for before, after in changes:
    if text.count(after) == 1:
        continue
    if text.count(before) != 1:
        raise SystemExit("Pinned FFmpeg HLS source differs from the expected version")
    text = text.replace(before, after, 1)
if text != path.read_text():
    path.write_text(text)
