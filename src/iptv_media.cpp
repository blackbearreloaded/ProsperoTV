// ProsperoTV - Bounded container demux, reusing the native MPEG-TS player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_media.h"
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace iptv
{
namespace
{
constexpr std::int64_t kMaxFileBytes = INT64_C(1) << 40;
constexpr int kBufferBytes = 64 * 1024;
constexpr std::size_t kMaxWebVttBytes = 1024 * 1024;
bool vtt_time(std::string_view text, std::int64_t &milliseconds)
{
    // mm:ss.mmm or hh:mm:ss.mmm; hours may exceed two digits.
    const auto colon = text.find(':'), last = text.rfind(':');
    if (colon == text.npos || text.size() - last != 7 || text[last + 3] != '.')
        return false;
    const auto number = [](std::string_view s, std::int64_t &value)
    {
        if (s.empty() || s.size() > 8 ||
            std::any_of(s.begin(), s.end(), [](char c) { return c < '0' || c > '9'; }))
            return false;
        const auto parsed = std::from_chars(s.data(), s.data() + s.size(), value);
        return parsed.ec == std::errc{} && parsed.ptr == s.data() + s.size();
    };
    std::int64_t hours = 0, minutes = 0, seconds = 0, fraction = 0;
    if (colon != last && (colon < 2 || last - colon != 3 || !number(text.substr(0, colon), hours)))
        return false;
    const auto min = colon == last ? text.substr(0, colon) : text.substr(colon + 1, 2);
    if (min.size() != 2 || !number(min, minutes) || minutes >= 60 ||
        !number(text.substr(last + 1, 2), seconds) || seconds >= 60 ||
        !number(text.substr(last + 4, 3), fraction))
        return false;
    milliseconds = ((hours * 60 + minutes) * 60 + seconds) * 1000 + fraction;
    return true;
}
std::string vtt_timestamp(std::int64_t ms)
{
    char text[40];
    std::snprintf(text, sizeof(text), "%02lld:%02lld:%02lld.%03lld",
                  static_cast<long long>(ms / 3600000), static_cast<long long>(ms / 60000 % 60),
                  static_cast<long long>(ms / 1000 % 60), static_cast<long long>(ms % 1000));
    return text;
}
struct Resource
{
    struct Location
    {
        const AVClass *av_class;
        const char *location;
    };
    static const AVClass *location_class()
    {
        static const AVOption options[] = {{"location",
                                            "Final resource URL",
                                            offsetof(Location, location),
                                            AV_OPT_TYPE_STRING,
                                            {.str = nullptr},
                                            0,
                                            0,
                                            AV_OPT_FLAG_READONLY,
                                            nullptr},
                                           {}};
        static const AVClass type = []
        {
            AVClass value{};
            value.class_name = "ProsperoTV HTTP location";
            value.item_name = av_default_item_name;
            value.version = LIBAVUTIL_VERSION_INT;
            value.option = options;
            return value;
        }();
        return &type;
    }
    static const AVClass *io_class()
    {
        static const AVClass type = []
        {
            AVClass value{};
            value.class_name = "ProsperoTV HTTP reader";
            value.item_name = av_default_item_name;
            value.version = LIBAVUTIL_VERSION_INT;
            value.child_next = [](void *self, void *previous) -> void *
            {
                auto *r = static_cast<Resource *>(static_cast<AVIOContext *>(self)->opaque);
                return previous || !r->location.location ? nullptr : &r->location;
            };
            return value;
        }();
        return &type;
    }
    MediaInput input;
    AVIOContext *io = nullptr;
    Location location{location_class(), nullptr};
    std::string prefix;
    std::size_t at = 0;
    bool initialized = false, buffered = false;
    ~Resource()
    {
        if (io)
            av_freep(&io->buffer);
        avio_context_free(&io);
        if (input.close)
            input.close(input.context);
    }
    int raw(std::uint8_t *bytes, int count)
    {
        if (input.cancelled && input.cancelled(input.context))
            return AVERROR_EXIT;
        const int result = input.read(input.context, bytes, count);
        return result < 0 || result > count ? AVERROR(EIO) : result ? result : AVERROR_EOF;
    }
    static int read(void *self, std::uint8_t *bytes, int count)
    {
        auto &r = *static_cast<Resource *>(self);
        if (count <= 0)
            return AVERROR(EINVAL);
        if (r.input.cancelled && r.input.cancelled(r.input.context))
            return AVERROR_EXIT;
        if (!r.initialized)
        {
            std::uint8_t chunk[4096];
            while (r.prefix.size() < 9)
            {
                const int got = r.raw(chunk, static_cast<int>(9 - r.prefix.size()));
                if (got == AVERROR_EOF)
                    break;
                if (got < 0)
                    return got;
                r.prefix.append(reinterpret_cast<char *>(chunk), got);
            }
            const bool vtt =
                r.prefix.starts_with("WEBVTT") || r.prefix.starts_with("\xef\xbb\xbfWEBVTT");
            r.buffered = vtt || r.prefix.starts_with("#EXTM3U");
            if (r.buffered)
            {
                for (;;)
                {
                    const int got = r.raw(chunk, sizeof(chunk));
                    if (got == AVERROR_EOF)
                        break;
                    if (got < 0)
                        return got;
                    if (r.prefix.size() + got > (vtt ? kMaxWebVttBytes : 256u * 1024u))
                        return AVERROR(EFBIG);
                    r.prefix.append(reinterpret_cast<char *>(chunk), got);
                }
                if (vtt)
                {
                    std::string normalized;
                    if (!NormalizeHlsWebVtt(r.prefix, normalized))
                        return AVERROR_INVALIDDATA;
                    r.prefix = std::move(normalized);
                }
            }
            r.initialized = true;
        }
        if (r.at < r.prefix.size())
        {
            const auto got = std::min<std::size_t>(count, r.prefix.size() - r.at);
            std::memcpy(bytes, r.prefix.data() + r.at, got);
            r.at += got;
            return static_cast<int>(got);
        }
        return r.buffered ? AVERROR_EOF : r.raw(bytes, count);
    }
};
struct Reader
{
    const MediaInput &input;
    const MediaOutput &output;
    std::int64_t position = 0;
    AVFormatContext *demux = nullptr, *mux = nullptr;
    AVIOContext *in = nullptr, *out = nullptr;
    AVPacket *packet = nullptr;
    std::vector<std::unique_ptr<Resource>> resources{};
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
    static int open_resource(AVFormatContext *context, AVIOContext **out, const char *url,
                             int flags, AVDictionary **options)
    {
        auto &r = *static_cast<Reader *>(context->opaque);
        const std::string_view address = url ? url : "";
        if (flags != AVIO_FLAG_READ || !r.input.open_resource || r.resources.size() >= 64 ||
            address.size() > 4096 ||
            (!address.starts_with("http://") && !address.starts_with("https://")) ||
            std::any_of(address.begin(), address.end(),
                        [](unsigned char c) { return c <= 32 || c == 127; }))
            return AVERROR(EACCES);
        if (r.cancelled())
            return AVERROR_EXIT;
        std::int64_t begin = 0, end = -1;
        const auto range = [&](const char *name, std::int64_t &value)
        {
            const auto *entry = options ? av_dict_get(*options, name, nullptr, 0) : nullptr;
            if (!entry)
                return true;
            const char *last = entry->value + std::strlen(entry->value);
            const auto parsed = std::from_chars(entry->value, last, value);
            return parsed.ec == std::errc{} && parsed.ptr == last && value >= 0 &&
                   value <= kMaxFileBytes;
        };
        if (!range("offset", begin) || !range("end_offset", end) || (end >= 0 && end <= begin))
            return AVERROR(EINVAL);
        auto resource = std::make_unique<Resource>();
        if (!r.input.open_resource(r.input.context, url, begin, end, &resource->input) ||
            !resource->input.read || !resource->input.close)
            return AVERROR(EIO);
        auto *buffer = static_cast<std::uint8_t *>(av_malloc(kBufferBytes));
        if (!buffer)
            return AVERROR(ENOMEM);
        resource->io = avio_alloc_context(buffer, kBufferBytes, 0, resource.get(), Resource::read,
                                          nullptr, nullptr);
        if (!resource->io)
        {
            av_free(buffer);
            return AVERROR(ENOMEM);
        }
        resource->io->seekable = 0;
        resource->io->av_class = Resource::io_class();
        resource->location.location = resource->input.url;
        *out = resource->io;
        r.resources.push_back(std::move(resource));
        return 0;
    }
    static int close_resource(AVFormatContext *context, AVIOContext *io)
    {
        auto &r = *static_cast<Reader *>(context->opaque);
        const auto found = std::find_if(r.resources.begin(), r.resources.end(),
                                        [&](const auto &resource) { return resource->io == io; });
        if (found == r.resources.end())
            return AVERROR(EINVAL);
        r.resources.erase(found);
        return 0;
    }
};
bool audio_supported(AVCodecID codec)
{
    return codec == AV_CODEC_ID_AAC || codec == AV_CODEC_ID_MP2 || codec == AV_CODEC_ID_MP3 ||
           codec == AV_CODEC_ID_AC3 || codec == AV_CODEC_ID_EAC3;
}
} // namespace
bool NormalizeHlsWebVtt(std::string_view segment, std::string &normalized)
{
    normalized.clear();
    if (segment.size() > kMaxWebVttBytes || segment.find('\0') != segment.npos)
        return false;
    if (segment.starts_with("\xef\xbb\xbf"))
        segment.remove_prefix(3);
    if (!segment.starts_with("WEBVTT") ||
        (segment.size() > 6 && std::string_view(" \t\r\n").find(segment[6]) == segment.npos))
        return false;
    const auto line = [&]()
    {
        const auto end = segment.find_first_of("\r\n");
        const auto text = segment.substr(0, end);
        if (end == segment.npos)
            segment = {};
        else
        {
            const bool crlf =
                segment[end] == '\r' && end + 1 < segment.size() && segment[end + 1] == '\n';
            segment.remove_prefix(end + (crlf ? 2 : 1));
        }
        return text;
    };
    (void)line();
    constexpr std::int64_t wrap = INT64_C(1) << 33;
    std::int64_t local = 0, transport = 0;
    bool mapped = false, header = true, block_start = true, ignore_block = false, cue_timed = false;
    unsigned cues = 0;
    normalized = "WEBVTT\n\n";
    while (!segment.empty())
    {
        const auto text = line();
        if (text.size() > 16 * 1024)
            return false;
        if (header)
        {
            if (text.empty())
                header = false;
            else if (text.starts_with("X-TIMESTAMP-MAP="))
            {
                if (mapped)
                    return false;
                auto fields = text.substr(16);
                bool have_local = false, have_transport = false;
                while (!fields.empty())
                {
                    const auto comma = fields.find(',');
                    const auto field = fields.substr(0, comma);
                    if (field.starts_with("LOCAL:") && !have_local)
                    {
                        if (!vtt_time(field.substr(6), local))
                            return false;
                        have_local = true;
                    }
                    else if (field.starts_with("MPEGTS:") && !have_transport)
                    {
                        const auto digits = field.substr(7);
                        const auto parsed = std::from_chars(
                            digits.data(), digits.data() + digits.size(), transport);
                        if (parsed.ec != std::errc{} ||
                            parsed.ptr != digits.data() + digits.size() || transport < 0 ||
                            transport >= wrap)
                            return false;
                        have_transport = true;
                    }
                    else
                        return false;
                    if (comma == fields.npos)
                        break;
                    fields.remove_prefix(comma + 1);
                }
                if (!have_local || !have_transport)
                    return false;
                mapped = true;
            }
            continue;
        }
        if (text.empty())
        {
            block_start = true;
            ignore_block = false;
            cue_timed = false;
        }
        else if (block_start)
        {
            ignore_block = text == "NOTE" || text.starts_with("NOTE ") ||
                           text.starts_with("NOTE\t") || text == "STYLE" || text == "REGION";
            block_start = false;
        }
        const auto arrow = text.find(" --> ");
        if (!ignore_block && !cue_timed && arrow != text.npos)
        {
            const auto after = text.substr(arrow + 5);
            const auto settings = after.find_first_of(" \t");
            std::int64_t start = 0, end = 0;
            if (++cues > 4096 || !vtt_time(text.substr(0, arrow), start) ||
                !vtt_time(after.substr(0, settings), end) || end < start || end - start > 600000)
                return false;
            cue_timed = true;
            auto ticks = (transport + (start - local) * 90) % wrap;
            if (ticks < 0)
                ticks += wrap;
            const auto mapped_start = (ticks + 45) / 90;
            normalized +=
                vtt_timestamp(mapped_start) + " --> " + vtt_timestamp(mapped_start + end - start);
            if (settings != after.npos)
                normalized += after.substr(settings);
        }
        else
            normalized += text;
        normalized += '\n';
        if (normalized.size() > 2 * kMaxWebVttBytes)
            return false;
    }
    return true;
}
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
    const bool hls = input.url && input.open_resource;
    // Library diagnostics can include complete provider URLs. The application
    // reports bounded errors of its own; do not copy those URLs into stderr.
    static std::once_flag log_policy;
    std::call_once(log_policy,
                   []
                   {
                       av_log_set_level(AV_LOG_QUIET);
                       av_max_alloc(64u * 1024u * 1024u);
                   });
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
    r.demux->opaque = &r;
    r.demux->io_open = hls ? Reader::open_resource : Reader::deny_open;
    if (hls)
        r.demux->io_close2 = Reader::close_resource;
    r.demux->interrupt_callback = {Reader::interrupt, &r};
    r.demux->probesize = 4 * 1024 * 1024;
    r.demux->max_analyze_duration = 5 * AV_TIME_BASE;
    // Provider movies commonly carry dozens of subtitle languages. Keep the
    // demux bound separate from the smaller number of tracks exposed by the UI.
    constexpr unsigned kMaxContainerStreams = 128;
    r.demux->max_streams = kMaxContainerStreams;
    r.demux->max_index_size = 8 * 1024 * 1024;
    AVDictionary *options = nullptr;
    av_dict_set(&options, "format_whitelist",
                hls ? "hls,mpegts,mov,matroska,webm,aac,ac3,eac3,mp3,webvtt" : "mov,matroska,webm",
                0);
    av_dict_set(&options, "protocol_whitelist", "", 0);
    if (hls)
    {
        // All HTTP lifetime/range handling belongs to the application's callback.
        av_dict_set(&options, "http_persistent", "0", 0);
        av_dict_set(&options, "http_multiple", "0", 0);
        av_dict_set(&options, "http_seekable", "0", 0);
        av_dict_set(&options, "max_reload", "8", 0);
        av_dict_set(&options, "m3u8_hold_counters", "8", 0);
        av_dict_set(&options, "seg_format_options", "threads=1", 0);
    }
    const int opened = avformat_open_input(&r.demux, hls ? input.url : nullptr,
                                           hls ? av_find_input_format("hls") : nullptr, &options);
    av_dict_free(&options);
    if (r.cancelled())
        return 1;
    if (opened < 0 || r.demux->nb_streams > kMaxContainerStreams)
        return r.cancelled() ? 1
                             : fail(hls ? "This HLS video could not be read."
                                        : "This MP4 or Matroska video could not be read.");
    if (hls)
        for (unsigned i = 0; i < r.demux->nb_streams; ++i)
            if (r.demux->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
                r.demux->streams[i]->discard = AVDISCARD_DEFAULT;
    std::vector<AVDictionary *> probe_options(r.demux->nb_streams, nullptr);
    for (auto &option : probe_options)
        av_dict_set(&option, "threads", "1", 0);
    const int info = avformat_find_stream_info(r.demux, probe_options.data());
    for (auto &option : probe_options)
        av_dict_free(&option);
    if (info < 0)
        return r.cancelled() ? 1
                             : fail(hls ? "This HLS video could not be read."
                                        : "This MP4 or Matroska video could not be read.");
    int video = -1;
    std::vector<int> tracks;
    std::vector<SubtitleTrack> subtitles;
    for (unsigned i = 0; i < r.demux->nb_streams; ++i)
    {
        const auto *p = r.demux->streams[i]->codecpar;
        if (video < 0 && p->codec_type == AVMEDIA_TYPE_VIDEO &&
            !(r.demux->streams[i]->disposition & AV_DISPOSITION_ATTACHED_PIC) &&
            (p->codec_id == AV_CODEC_ID_H264 || p->codec_id == AV_CODEC_ID_HEVC))
            video = static_cast<int>(i);
        if (p->codec_type == AVMEDIA_TYPE_AUDIO && audio_supported(p->codec_id))
            tracks.push_back(static_cast<int>(i));
        if (p->codec_type == AVMEDIA_TYPE_SUBTITLE &&
            subtitle_codec(p->codec_id) != SubtitleCodec::none && p->extradata_size >= 0 &&
            p->extradata_size <= 64 * 1024)
        {
            const auto *stream = r.demux->streams[i];
            SubtitleTrack track;
            track.info.id = i + 1;
            track.info.codec = subtitle_codec(p->codec_id);
            track.info.forced = (stream->disposition & AV_DISPOSITION_FORCED) != 0;
            track.info.hearing_impaired =
                (stream->disposition & AV_DISPOSITION_HEARING_IMPAIRED) != 0;
            if (const auto *language = av_dict_get(stream->metadata, "language", nullptr, 0))
                track.info.language.assign(language->value, strnlen(language->value, 32));
            if (const auto *title = av_dict_get(stream->metadata, "title", nullptr, 0))
                track.info.title.assign(title->value, strnlen(title->value, 128));
            else if (const auto *name = av_dict_get(stream->metadata, "comment", nullptr, 0))
                track.info.title.assign(name->value, strnlen(name->value, 128));
            if (p->extradata_size && p->extradata)
                track.extra.assign(p->extradata, p->extradata + p->extradata_size);
            track.width = static_cast<unsigned>(std::max(0, p->width));
            track.height = static_cast<unsigned>(std::max(0, p->height));
            subtitles.push_back(std::move(track));
            if (hls)
                r.demux->streams[i]->discard = AVDISCARD_DEFAULT;
        }
    }
    if (video < 0)
        return fail("This video needs an H.264 or HEVC video track.");
    if (hls)
        std::stable_sort(tracks.begin(), tracks.end(),
                         [&](int a, int b)
                         {
                             return (r.demux->streams[a]->disposition & AV_DISPOSITION_DEFAULT) >
                                    (r.demux->streams[b]->disposition & AV_DISPOSITION_DEFAULT);
                         });
    const auto *v = r.demux->streams[video]->codecpar;
    if (v->width <= 0 || v->height <= 0 || v->width > 3840 || v->height > 2160)
        return fail("This video's resolution is not supported.");
    if (avformat_alloc_output_context2(&r.mux, nullptr, "mpegts", nullptr) < 0 || !r.mux)
        return fail("The video transport could not start.");
    // Give video, audio and separate subtitle packets one explicit timeline.
    // A positive lead-in accommodates normal B-frame decoding timestamps.
    const auto origin = r.demux->start_time == AV_NOPTS_VALUE ? INT64_C(0) : r.demux->start_time;
    if (origin < INT64_C(2000000) - std::numeric_limits<std::int64_t>::max())
        return fail("This video's timestamps are not supported.");
    const auto timestamp_offset = INT64_C(2000000) - origin;
    r.mux->output_ts_offset = timestamp_offset;
    r.mux->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
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
    std::vector<MediaAudioTrack> audio_metadata;
    tracks.insert(tracks.begin(), video);
    for (const int index : tracks)
    {
        if (index < 0)
            continue;
        auto *stream = avformat_new_stream(r.mux, nullptr);
        if (!stream ||
            avcodec_parameters_copy(stream->codecpar, r.demux->streams[index]->codecpar) < 0)
            return fail("The video's tracks could not be prepared.");
        stream->codecpar->codec_tag = 0;
        stream->id = 0x100 + stream->index;
        stream->time_base = r.demux->streams[index]->time_base;
        stream->disposition = r.demux->streams[index]->disposition;
        if (const auto *language =
                av_dict_get(r.demux->streams[index]->metadata, "language", nullptr, 0))
        {
            // MPEG-TS carries three-letter ISO 639 language codes.
            char code[4]{};
            if (std::strlen(language->value) == 3 &&
                std::all_of(language->value, language->value + 3, [](char c)
                            { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }))
            {
                std::memcpy(code, language->value, 3);
                av_dict_set(&stream->metadata, "language", code, 0);
            }
        }
        mapping[index] = stream->index;
        if (index != video)
        {
            MediaAudioTrack track;
            track.pid = static_cast<std::uint32_t>(stream->id);
            track.audio_type = stream->disposition & AV_DISPOSITION_HEARING_IMPAIRED  ? 2
                               : stream->disposition & AV_DISPOSITION_VISUAL_IMPAIRED ? 3
                                                                                      : 0;
            const auto *metadata = r.demux->streams[index]->metadata;
            if (const auto *language = av_dict_get(metadata, "language", nullptr, 0))
                track.language.assign(language->value, strnlen(language->value, 31));
            if (const auto *title = av_dict_get(metadata, "title", nullptr, 0))
                track.title.assign(title->value, strnlen(title->value, 127));
            else if (const auto *name = av_dict_get(metadata, "comment", nullptr, 0))
                track.title.assign(name->value, strnlen(name->value, 127));
            audio_metadata.push_back(std::move(track));
        }
    }
    AVDictionary *mux_options = nullptr;
    av_dict_set(&mux_options, "mpegts_copyts", "1", 0);
    const int header = avformat_write_header(r.mux, &mux_options);
    av_dict_free(&mux_options);
    if (header < 0)
        return fail("This video's tracks are not supported.");
    if (output.subtitle_tracks)
        output.subtitle_tracks(output.context, subtitles);
    if (output.audio_tracks)
        output.audio_tracks(output.context, audio_metadata);
    r.packet = av_packet_alloc();
    if (!r.packet)
        return fail("Not enough memory to open this video.");
    int result = 0;
    auto video_clock = origin;
    while (!r.cancelled() && (result = av_read_frame(r.demux, r.packet)) >= 0)
    {
        const int source = r.packet->stream_index;
        if (source < 0 || static_cast<std::size_t>(source) >= mapping.size() ||
            r.packet->size < 0 || r.packet->size > 8 * 1024 * 1024)
            return fail("The video contains an invalid media packet.");
        if (mapping[source] >= 0)
        {
            if (source == video && r.packet->pts != AV_NOPTS_VALUE)
                video_clock = av_rescale_q(r.packet->pts, r.demux->streams[source]->time_base,
                                           AVRational{1, AV_TIME_BASE});
            r.packet->stream_index = mapping[source];
            av_packet_rescale_ts(r.packet, r.demux->streams[source]->time_base,
                                 r.mux->streams[r.packet->stream_index]->time_base);
            r.packet->pos = -1;
            // Packets already arrive in demux order. Avoid an unbounded
            // interleave queue when a provider's audio timestamps are broken.
            if (av_write_frame(r.mux, r.packet) < 0 || r.out->error < 0)
                return r.cancelled() ? 1 : fail("The video's media packets could not be played.");
        }
        else if (output.subtitle_packet && r.packet->pts != AV_NOPTS_VALUE &&
                 std::any_of(subtitles.begin(), subtitles.end(), [&](const auto &track)
                             { return track.info.id == static_cast<unsigned>(source) + 1; }))
        {
            auto base = av_rescale_q(r.packet->pts, r.demux->streams[source]->time_base,
                                     AVRational{1, AV_TIME_BASE});
            if (hls && r.demux->streams[source]->codecpar->codec_id == AV_CODEC_ID_WEBVTT &&
                base > INT64_MIN / 4 && base < INT64_MAX / 4 && video_clock > INT64_MIN / 4 &&
                video_clock < INT64_MAX / 4)
            {
                // Choose the video's epoch when a WebVTT map crosses the 33-bit
                // MPEG clock wrap. Millisecond cue rounding is at most 0.5 ms.
                constexpr auto wrap_us = ((INT64_C(1) << 33) * AV_TIME_BASE + 45000) / 90000;
                const auto delta = video_clock - base;
                auto epochs = delta / wrap_us;
                if (delta % wrap_us > wrap_us / 2)
                    ++epochs;
                else if (delta % wrap_us < -wrap_us / 2)
                    --epochs;
                base += epochs * wrap_us;
            }
            const auto duration =
                av_rescale_q(r.packet->duration, r.demux->streams[source]->time_base,
                             AVRational{1, AV_TIME_BASE});
            if ((timestamp_offset >= 0 && base <= INT64_MAX - timestamp_offset) ||
                (timestamp_offset < 0 && base >= INT64_MIN - timestamp_offset))
                output.subtitle_packet(output.context, static_cast<unsigned>(source) + 1,
                                       r.packet->data, static_cast<std::size_t>(r.packet->size),
                                       base + timestamp_offset, duration);
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
