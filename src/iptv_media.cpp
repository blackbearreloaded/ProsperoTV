// ProsperoTV - Bounded container demux, reusing the native MPEG-TS player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_media.h"
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <limits>
#include <vector>

namespace iptv
{
namespace
{
constexpr std::int64_t kMaxFileBytes = INT64_C(1) << 40;
constexpr int kBufferBytes = 64 * 1024;
struct Reader
{
    const MediaInput &input;
    const MediaOutput &output;
    std::int64_t position = 0;
    AVFormatContext *demux = nullptr, *mux = nullptr;
    AVIOContext *in = nullptr, *out = nullptr;
    AVPacket *packet = nullptr;
    ~Reader()
    {
        av_packet_free(&packet);
        avformat_close_input(&demux);
        if (mux)
            mux->pb = nullptr;
        avformat_free_context(mux);
        if (in)
            av_freep(&in->buffer);
        if (out)
            av_freep(&out->buffer);
        avio_context_free(&in);
        avio_context_free(&out);
    }
    bool cancelled() const
    {
        return input.cancelled && input.cancelled(input.context);
    }
    static int interrupt(void *self)
    {
        return static_cast<Reader *>(self)->cancelled();
    }
    static int read(void *self, std::uint8_t *buffer, int bytes)
    {
        auto &r = *static_cast<Reader *>(self);
        if (r.cancelled())
            return AVERROR_EXIT;
        if (bytes <= 0 || r.position > kMaxFileBytes - bytes)
            return AVERROR(EFBIG);
        const int got = r.input.read(r.input.context, buffer, bytes);
        if (got < 0 || got > bytes)
            return AVERROR(EIO);
        r.position += got;
        return got ? got : AVERROR_EOF;
    }
    static std::int64_t seek(void *self, std::int64_t offset, int whence)
    {
        auto &r = *static_cast<Reader *>(self);
        if (r.cancelled())
            return AVERROR_EXIT;
        const auto size = r.input.size ? r.input.size(r.input.context) : -1;
        if (whence == AVSEEK_SIZE)
            return size >= 0 && size <= kMaxFileBytes ? size : AVERROR(ENOSYS);
        whence &= ~AVSEEK_FORCE;
        const auto base = whence == SEEK_SET   ? 0
                          : whence == SEEK_CUR ? r.position
                          : whence == SEEK_END ? size
                                               : -1;
        if (base < 0 || offset < -base || offset > kMaxFileBytes - base || !r.input.seek)
            return AVERROR(EINVAL);
        const auto target = base + offset;
        if (size >= 0 && target > size)
            return AVERROR(EINVAL);
        if (target == r.position)
            return target;
        const auto result = r.input.seek(r.input.context, target);
        if (result != target)
            return AVERROR(EIO);
        return r.position = target;
    }
    static int write(void *self, const std::uint8_t *bytes, int count)
    {
        auto &r = *static_cast<Reader *>(self);
        if (r.cancelled())
            return AVERROR_EXIT;
        return count >= 0 &&
                       r.output.write(r.output.context, bytes, static_cast<std::size_t>(count))
                   ? count
                   : AVERROR(EIO);
    }
    static int deny_open(AVFormatContext *, AVIOContext **, const char *, int, AVDictionary **)
    {
        return AVERROR(EACCES);
    }
};
bool audio_supported(AVCodecID codec)
{
    return codec == AV_CODEC_ID_AAC || codec == AV_CODEC_ID_MP2 || codec == AV_CODEC_ID_MP3 ||
           codec == AV_CODEC_ID_AC3 || codec == AV_CODEC_ID_EAC3;
}
} // namespace
bool LooksLikeMedia(const std::uint8_t *bytes, std::size_t count)
{
    if (!bytes || count < 8)
        return false;
    if (bytes[0] == 0x1a && bytes[1] == 0x45 && bytes[2] == 0xdf && bytes[3] == 0xa3)
        return true;
    for (const auto *box : {"ftyp", "styp", "moov", "mdat", "free", "wide"})
        if (std::memcmp(bytes + 4, box, 4) == 0)
            return true;
    return false;
}
int ReadMedia(const MediaInput &input, const MediaOutput &output, std::string *error)
{
    const auto fail = [&](const char *message)
    {
        if (error)
            *error = message;
        return -1;
    };
    if (error)
        error->clear();
    if (!input.read || !output.write)
        return fail("The media reader could not start.");
    av_max_alloc(64u * 1024u * 1024u);
    Reader r{input, output};
    auto *buffer = static_cast<std::uint8_t *>(av_malloc(kBufferBytes));
    if (!buffer)
        return fail("Not enough memory to open this video.");
    r.in = avio_alloc_context(buffer, kBufferBytes, 0, &r, Reader::read, nullptr, Reader::seek);
    if (!r.in)
    {
        av_free(buffer);
        return fail("Not enough memory to open this video.");
    }
    r.in->seekable = input.seek ? AVIO_SEEKABLE_NORMAL : 0;
    r.demux = avformat_alloc_context();
    if (!r.demux)
        return fail("Not enough memory to open this video.");
    r.demux->pb = r.in;
    r.demux->flags |= AVFMT_FLAG_CUSTOM_IO;
    r.demux->io_open = Reader::deny_open;
    r.demux->interrupt_callback = {Reader::interrupt, &r};
    r.demux->probesize = 4 * 1024 * 1024;
    r.demux->max_analyze_duration = 5 * AV_TIME_BASE;
    r.demux->max_streams = 32;
    r.demux->max_index_size = 8 * 1024 * 1024;
    AVDictionary *options = nullptr;
    av_dict_set(&options, "format_whitelist", "mov,matroska,webm", 0);
    av_dict_set(&options, "protocol_whitelist", "", 0);
    const int opened = avformat_open_input(&r.demux, nullptr, nullptr, &options);
    av_dict_free(&options);
    if (r.cancelled())
        return 1;
    if (opened < 0 || avformat_find_stream_info(r.demux, nullptr) < 0)
        return r.cancelled() ? 1 : fail("This MP4 or Matroska video could not be read.");
    int video = -1, audio = -1;
    for (unsigned i = 0; i < r.demux->nb_streams; ++i)
    {
        const auto *p = r.demux->streams[i]->codecpar;
        if (video < 0 && p->codec_type == AVMEDIA_TYPE_VIDEO &&
            !(r.demux->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) &&
            (p->codec_id == AV_CODEC_ID_H264 || p->codec_id == AV_CODEC_ID_HEVC))
            video = static_cast<int>(i);
        if (audio < 0 && p->codec_type == AVMEDIA_TYPE_AUDIO && audio_supported(p->codec_id))
            audio = static_cast<int>(i);
    }
    if (video < 0)
        return fail("This video needs an H.264 or HEVC video track.");
    const auto *v = r.demux->streams[video]->codecpar;
    if (v->width <= 0 || v->height <= 0 || v->width > 3840 || v->height > 2160)
        return fail("This video's resolution is not supported.");
    if (avformat_alloc_output_context2(&r.mux, nullptr, "mpegts", nullptr) < 0 || !r.mux)
        return fail("The video transport could not start.");
    buffer = static_cast<std::uint8_t *>(av_malloc(kBufferBytes));
    if (!buffer)
        return fail("Not enough memory to open this video.");
    r.out = avio_alloc_context(buffer, kBufferBytes, 1, &r, nullptr, Reader::write, nullptr);
    if (!r.out)
    {
        av_free(buffer);
        return fail("Not enough memory to open this video.");
    }
    r.mux->pb = r.out;
    r.mux->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_AUTO_BSF;
    r.mux->max_interleave_delta = AV_TIME_BASE;
    std::vector<int> mapping(r.demux->nb_streams, -1);
    for (const int index : {video, audio})
    {
        if (index < 0)
            continue;
        auto *stream = avformat_new_stream(r.mux, nullptr);
        if (!stream ||
            avcodec_parameters_copy(stream->codecpar, r.demux->streams[index]->codecpar) < 0)
            return fail("The video's tracks could not be prepared.");
        stream->codecpar->codec_tag = 0;
        stream->time_base = r.demux->streams[index]->time_base;
        mapping[index] = stream->index;
    }
    if (avformat_write_header(r.mux, nullptr) < 0)
        return fail("This video's tracks are not supported.");
    r.packet = av_packet_alloc();
    if (!r.packet)
        return fail("Not enough memory to open this video.");
    int result = 0;
    while (!r.cancelled() && (result = av_read_frame(r.demux, r.packet)) >= 0)
    {
        const int source = r.packet->stream_index;
        if (source < 0 || static_cast<std::size_t>(source) >= mapping.size() ||
            r.packet->size < 0 || r.packet->size > 8 * 1024 * 1024)
            return fail("The video contains an invalid media packet.");
        if (mapping[source] >= 0)
        {
            r.packet->stream_index = mapping[source];
            av_packet_rescale_ts(r.packet, r.demux->streams[source]->time_base,
                                 r.mux->streams[r.packet->stream_index]->time_base);
            r.packet->pos = -1;
            // Packets already arrive in demux order. Avoid an unbounded
            // interleave queue when a provider's audio timestamps are broken.
            if (av_write_frame(r.mux, r.packet) < 0 || r.out->error < 0)
                return r.cancelled() ? 1 : fail("The video's media packets could not be played.");
        }
        av_packet_unref(r.packet);
    }
    if (r.cancelled())
        return 1;
    if (result != AVERROR_EOF)
        return fail("The video download ended unexpectedly.");
    if (av_write_trailer(r.mux) < 0)
        return fail("The end of the video could not be read.");
    avio_flush(r.out);
    return r.out->error < 0 ? fail("The video could not finish playing.") : 0;
}
} // namespace iptv
