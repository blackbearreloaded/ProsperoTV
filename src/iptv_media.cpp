// ProsperoTV - Bounded container demux, reusing the native MPEG-TS player.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_media.h"
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/crc.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
}
extern "C"
{
    // The interface's diagnostic log; absent in the tests.
    __attribute__((weak)) int tv_diag_enabled(void);
    __attribute__((weak)) void tv_diag_line(const char *line);
}
#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
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
constexpr int kTsPacketBytes = 188;
// Whole transport packets per write, so a programme table is never split across two.
constexpr int kMuxBufferBytes = kBufferBytes / kTsPacketBytes * kTsPacketBytes;
// FFmpeg's muxer gives DTS the Blu-ray stream type 0x82 without the "HDMV" registration, and
// the parser then takes 0x82 for SCTE-27 subtitles. Adds the registration to a PMT packet.
bool label_dts_programme(std::uint8_t *packet)
{
    if (packet[0] != 0x47 || !(packet[1] & 0x40) || (packet[3] & 0xc0))
        return false;
    const unsigned pid = static_cast<unsigned>((packet[1] & 0x1f) << 8 | packet[2]);
    if (pid <= 0x1f)
        return false; // PAT, SDT, NIT and the other reserved tables
    std::size_t at = 4;
    if (packet[3] & 0x20)
        at += 1u + packet[4];
    if (!(packet[3] & 0x10) || at >= kTsPacketBytes)
        return false;
    at += 1u + packet[at]; // the pointer field
    if (at + 16 > kTsPacketBytes || packet[at] != 0x02)
        return false;
    std::uint8_t *section = packet + at;
    const std::size_t room = kTsPacketBytes - at;
    std::size_t length = static_cast<std::size_t>((section[1] & 0x0f) << 8 | section[2]);
    std::size_t total = 3 + length; // with the CRC
    std::size_t info = static_cast<std::size_t>((section[10] & 0x0f) << 8 | section[11]);
    const std::uint8_t registration[6] = {0x05, 0x04, 'H', 'D', 'M', 'V'};
    if (total < 16 || total + sizeof(registration) > room || 12 + info + 4 > total)
        return false;
    for (std::size_t d = 12; d + 2 <= 12 + info; d += 2u + section[d + 1])
        if (section[d] == 0x05 && section[d + 1] >= 4 &&
            std::memcmp(section + d + 2, "HDMV", 4) == 0)
            return false; // labelled already
    bool dts = false;
    for (std::size_t e = 12 + info; e + 5 <= total - 4;
         e += 5u + static_cast<std::size_t>((section[e + 3] & 0x0f) << 8 | section[e + 4]))
        dts |= section[e] == 0x82;
    if (!dts)
        return false;
    std::memmove(section + 12 + info + sizeof(registration), section + 12 + info,
                 total - 4 - (12 + info));
    std::memcpy(section + 12 + info, registration, sizeof(registration));
    info += sizeof(registration);
    length += sizeof(registration);
    total += sizeof(registration);
    section[10] = static_cast<std::uint8_t>((section[10] & 0xf0) | (info >> 8));
    section[11] = static_cast<std::uint8_t>(info & 0xff);
    section[1] = static_cast<std::uint8_t>((section[1] & 0xf0) | (length >> 8));
    section[2] = static_cast<std::uint8_t>(length & 0xff);
    // The CRC as the muxer writes it: the MPEG-2 CRC-32 of the section before it.
    const std::uint32_t crc =
        av_crc(av_crc_get_table(AV_CRC_32_IEEE), UINT32_MAX, section, static_cast<int>(total - 4));
    for (unsigned i = 0; i < 4; ++i)
        section[total - 4 + i] = static_cast<std::uint8_t>(crc >> (8 * i));
    return true;
}
struct Reader;
bool reader_gives_way(Reader &reader);
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
    Reader *reader = nullptr;
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
        if ((input.cancelled && input.cancelled(input.context)) ||
            (reader && reader_gives_way(*reader)))
            return AVERROR_EXIT;
        const int result = input.read(input.context, bytes, count);
        if (result < 0 && reader && reader_gives_way(*reader))
            return AVERROR_EXIT;
        return result < 0 || result > count ? AVERROR(EIO) : result ? result : AVERROR_EOF;
    }
    static int read(void *self, std::uint8_t *bytes, int count)
    {
        auto &r = *static_cast<Resource *>(self);
        if (count <= 0)
            return AVERROR(EINVAL);
        if ((r.input.cancelled && r.input.cancelled(r.input.context)) ||
            (r.reader && reader_gives_way(*r.reader)))
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
    bool reading = false, gave_way = false;
    bool label_dts = false; // a DTS track goes out: its programme table needs the label
    ~Reader()
    {
        av_packet_free(&packet);
        avformat_close_input(&demux);
        close_mux();
        if (in)
            av_freep(&in->buffer);
        avio_context_free(&in);
    }
    void close_mux()
    {
        if (mux)
            mux->pb = nullptr;
        avformat_free_context(mux);
        mux = nullptr;
        if (out)
            av_freep(&out->buffer);
        avio_context_free(&out);
    }
    bool cancelled() const
    {
        return input.cancelled && input.cancelled(input.context);
    }
    bool gives_way()
    {
        if (reading && input.seek_waiting && input.seek_waiting(input.context))
            gave_way = true;
        return gave_way;
    }
    static int interrupt(void *self)
    {
        auto &r = *static_cast<Reader *>(self);
        return r.cancelled() || r.gives_way();
    }
    static int read(void *self, std::uint8_t *buffer, int bytes)
    {
        auto &r = *static_cast<Reader *>(self);
        if (r.cancelled() || r.gives_way())
            return AVERROR_EXIT;
        if (bytes <= 0 || r.position > kMaxFileBytes - bytes)
            return AVERROR(EFBIG);
        const int got = r.input.read(r.input.context, buffer, bytes);
        if (got < 0 && r.gives_way())
            return AVERROR_EXIT;
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
        if (count < 0)
            return AVERROR(EIO);
        const auto out = [&](const std::uint8_t *from, std::size_t size)
        { return !size || r.output.write(r.output.context, from, size); };
        if (!r.label_dts || count % kTsPacketBytes)
            return out(bytes, static_cast<std::size_t>(count)) ? count : AVERROR(EIO);
        std::size_t written = 0;
        for (std::size_t at = 0; at + kTsPacketBytes <= static_cast<std::size_t>(count);
             at += kTsPacketBytes)
        {
            std::uint8_t packet[kTsPacketBytes];
            std::memcpy(packet, bytes + at, sizeof(packet));
            if (!label_dts_programme(packet))
                continue;
            if (!out(bytes + written, at - written) || !out(packet, sizeof(packet)))
                return AVERROR(EIO);
            written = at + kTsPacketBytes;
        }
        return out(bytes + written, static_cast<std::size_t>(count) - written) ? count
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
        if (r.cancelled() || r.gives_way())
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
        resource->reader = &r;
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
bool reader_gives_way(Reader &reader)
{
    return reader.gives_way();
}
bool audio_supported(const AVCodecParameters &p)
{
    // DTS plays its core; DTS Express has none.
    return p.codec_id == AV_CODEC_ID_AAC || p.codec_id == AV_CODEC_ID_MP2 ||
           p.codec_id == AV_CODEC_ID_MP3 || p.codec_id == AV_CODEC_ID_AC3 ||
           p.codec_id == AV_CODEC_ID_EAC3 ||
           (p.codec_id == AV_CODEC_ID_DTS && p.profile != AV_PROFILE_DTS_EXPRESS);
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
int ReadMedia(const MediaInput &input, const MediaOutput &output, std::string *error,
              std::int64_t start_us)
{
    const auto fail = [&](const char *message)
    {
        if (error)
            *error = message;
        return -1;
    };
    if (error)
        error->clear();
    if (!input.read || !output.write || start_us < 0 || start_us > INT64_MAX / 4)
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
                hls ? "hls,mpegts,mov,matroska,webm,aac,ac3,eac3,mp3,webvtt"
                    : "mov,matroska,webm,mpegts",
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
            (p->codec_id == AV_CODEC_ID_H264 || p->codec_id == AV_CODEC_ID_HEVC ||
             (p->codec_id == AV_CODEC_ID_VP9 && output.vp9_packet)))
            video = static_cast<int>(i);
        if (p->codec_type == AVMEDIA_TYPE_AUDIO && audio_supported(*p))
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
    if (v->codec_id == AV_CODEC_ID_VP9 && v->profile != AV_PROFILE_UNKNOWN && v->profile != 0)
        return fail("This video needs VP9 profile 0.");
    // Give video, audio and separate subtitle packets one explicit timeline.
    // A positive lead-in accommodates normal B-frame decoding timestamps.
    const auto origin = r.demux->start_time == AV_NOPTS_VALUE ? INT64_C(0) : r.demux->start_time;
    if (origin < -INT64_MAX / 4 || origin > INT64_MAX / 4)
        return fail("This video's timestamps are not supported.");
    MediaTimeline timeline;
    if (r.demux->duration > 0 && r.demux->duration < INT64_MAX / 4)
        timeline.duration_us = r.demux->duration;
    timeline.seekable = timeline.duration_us > 0 && (input.seek || hls);
    for (unsigned i = 0; i < r.demux->nb_chapters && timeline.chapters.size() < 128; ++i)
    {
        const auto *chapter = r.demux->chapters[i];
        if (!chapter || chapter->time_base.num <= 0 || chapter->time_base.den <= 0)
            continue;
        const auto position =
            av_rescale_q(chapter->start, chapter->time_base, AVRational{1, AV_TIME_BASE});
        if (position < 0 || position > INT64_MAX / 4 ||
            (timeline.duration_us && position >= timeline.duration_us))
            continue;
        MediaChapter item{position, {}};
        if (const auto *title = av_dict_get(chapter->metadata, "title", nullptr, 0))
            item.title.assign(title->value, strnlen(title->value, 128));
        timeline.chapters.push_back(std::move(item));
    }
    std::stable_sort(timeline.chapters.begin(), timeline.chapters.end(),
                     [](const auto &a, const auto &b) { return a.position_us < b.position_us; });
    if (output.timeline)
        output.timeline(output.context, timeline);
    const bool transport = !strcmp(r.demux->iformat->name, "mpegts");
    const auto seek_input = [&](std::int64_t start, const char **message)
    {
        const auto target = origin + std::min(start, timeline.duration_us - 1);
        // The TS demuxer's timestamp index marks every packet as a keyframe.
        // Locate a real preceding random-access picture before starting a fresh decoder.
        const auto probe = transport ? std::max(origin, target - INT64_C(30000000)) : target;
        const bool from_start = transport && probe == origin;
        const auto byte_seek = [&](std::int64_t position)
        {
            // FFmpeg's byte seek ignores a failed avio_seek, so move the input first.
            return avio_seek(r.in, position, SEEK_SET) >= 0 &&
                   av_seek_frame(r.demux, -1, position, AVSEEK_FLAG_BYTE) >= 0;
        };
        if (from_start ? !byte_seek(0)
                       : avformat_seek_file(r.demux, -1, INT64_MIN, probe, probe,
                                            AVSEEK_FLAG_BACKWARD) < 0)
        {
            *message = "The video could not seek to that position.";
            return r.cancelled() ? 1 : -1;
        }
        if (transport)
        {
            AVPacket *packet = av_packet_alloc();
            if (!packet)
            {
                *message = "Not enough memory to seek this video.";
                return -1;
            }
            std::int64_t position = -1;
            std::uint64_t scanned = 0;
            while (!r.cancelled() && scanned < UINT64_C(268435456) &&
                   av_read_frame(r.demux, packet) >= 0)
            {
                scanned += static_cast<unsigned>(std::max(0, packet->size));
                bool past = false;
                if (packet->stream_index == video && packet->pts != AV_NOPTS_VALUE)
                {
                    const auto pts = av_rescale_q(packet->pts, r.demux->streams[video]->time_base,
                                                  AVRational{1, AV_TIME_BASE});
                    if ((packet->flags & AV_PKT_FLAG_KEY) && packet->pos >= 0 && pts <= target)
                        position = packet->pos;
                    past = pts > target;
                }
                av_packet_unref(packet);
                if (past)
                    break;
            }
            av_packet_free(&packet);
            if (r.cancelled())
                return 1;
            if (position < 0 && from_start)
                position = 0;
            if (position < 0 || !byte_seek(position))
            {
                *message = "No nearby keyframe was found for this position.";
                return -1;
            }
        }
        return 0;
    };
    if (start_us)
    {
        if (!timeline.seekable)
            return fail("This video does not support seeking.");
        const char *message = "";
        const int sought = seek_input(start_us, &message);
        if (sought)
            return sought > 0 ? 1 : fail(message);
    }
    const auto timestamp_offset = INT64_C(2000000) - origin;
    const auto shift = [](std::int64_t &stamp, std::int64_t delta)
    {
        if (stamp == AV_NOPTS_VALUE)
            return true;
        if ((delta > 0 && stamp > INT64_MAX - delta) || (delta < 0 && stamp < INT64_MIN - delta))
            return false;
        stamp += delta;
        return stamp != AV_NOPTS_VALUE;
    };
    if (v->codec_id == AV_CODEC_ID_VP9)
    {
        r.packet = av_packet_alloc();
        if (!r.packet)
            return fail("Not enough memory to read this video.");
        int result = AVERROR_EOF;
        while (!r.cancelled() && (result = av_read_frame(r.demux, r.packet)) >= 0)
        {
            if (r.packet->stream_index == video)
            {
                if (r.packet->size <= 0 || r.packet->size > 8 * 1024 * 1024 ||
                    r.packet->pts == AV_NOPTS_VALUE)
                    return fail("This VP9 video packet is not supported.");
                auto pts = av_rescale_q(r.packet->pts, r.demux->streams[video]->time_base,
                                        AVRational{1, AV_TIME_BASE});
                if (!shift(pts, timestamp_offset) || pts < 0)
                    return fail("This video's timestamps are not supported.");
                const MediaVp9Packet packet{r.packet->data,
                                            static_cast<std::size_t>(r.packet->size),
                                            static_cast<std::uint64_t>(pts),
                                            static_cast<std::uint32_t>(v->width),
                                            static_cast<std::uint32_t>(v->height),
                                            (r.packet->flags & AV_PKT_FLAG_KEY) != 0};
                if (!output.vp9_packet(output.context, packet))
                    return r.cancelled() ? 1 : fail("The VP9 video could not continue playing.");
            }
            av_packet_unref(r.packet);
        }
        if (r.cancelled())
            return 1;
        return result == AVERROR_EOF ? 0 : fail("The video download ended unexpectedly.");
    }
    std::vector<int> mapping(r.demux->nb_streams, -1);
    tracks.insert(tracks.begin(), video);
    for (std::size_t k = 0; k < tracks.size(); ++k)
        mapping[tracks[k]] = static_cast<int>(k);
    // A fresh muxer per seek: one muxer for the whole video rejects a backward DTS.
    const auto open_mux = [&]() -> const char *
    {
        if (avformat_alloc_output_context2(&r.mux, nullptr, "mpegts", nullptr) < 0 || !r.mux)
            return "The video transport could not start.";
        r.mux->output_ts_offset = timestamp_offset;
        r.mux->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
        auto *output_buffer = static_cast<std::uint8_t *>(av_malloc(kMuxBufferBytes));
        if (!output_buffer)
            return "Not enough memory to open this video.";
        r.out = avio_alloc_context(output_buffer, kMuxBufferBytes, 1, &r, nullptr, Reader::write,
                                   nullptr);
        if (!r.out)
        {
            av_free(output_buffer);
            return "Not enough memory to open this video.";
        }
        r.mux->pb = r.out;
        r.mux->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_AUTO_BSF;
        r.mux->max_interleave_delta = AV_TIME_BASE;
        for (const int index : tracks)
        {
            auto *stream = avformat_new_stream(r.mux, nullptr);
            if (!stream ||
                avcodec_parameters_copy(stream->codecpar, r.demux->streams[index]->codecpar) < 0)
                return "The video's tracks could not be prepared.";
            stream->codecpar->codec_tag = 0;
            stream->id = 0x100 + stream->index;
            r.label_dts |= stream->codecpar->codec_id == AV_CODEC_ID_DTS;
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
        }
        AVDictionary *mux_options = nullptr;
        av_dict_set(&mux_options, "mpegts_copyts", "1", 0);
        const int header = avformat_write_header(r.mux, &mux_options);
        av_dict_free(&mux_options);
        return header < 0 ? "This video's tracks are not supported." : nullptr;
    };
    if (const char *message = open_mux())
        return fail(message);
    // For the lip-sync diagnostics: what each track's timeline starts with.
    const auto diag_enabled = [] { return tv_diag_enabled && tv_diag_line && tv_diag_enabled(); };
    const auto to_ms = [](std::int64_t stamp, AVRational base) -> long long
    { return stamp == AV_NOPTS_VALUE ? -1 : av_rescale_q(stamp, base, AVRational{1, 1000}); };
    if (diag_enabled())
    {
        for (const int index : tracks)
        {
            const auto *stream = r.demux->streams[index];
            const auto *p = stream->codecpar;
            char line[256];
            std::snprintf(line, sizeof(line),
                          "[player] media track %d: %s %s, %d Hz %d ch, starts %lld ms, priming "
                          "%d samples, container start %lld ms",
                          index, p->codec_type == AVMEDIA_TYPE_VIDEO ? "video" : "sound",
                          avcodec_get_name(p->codec_id), p->sample_rate, p->ch_layout.nb_channels,
                          to_ms(stream->start_time, stream->time_base), p->initial_padding,
                          (long long)(origin / 1000));
            tv_diag_line(line);
        }
    }
    std::vector<char> first_packet_traced(r.demux->nb_streams, 0);
    std::vector<MediaAudioTrack> audio_metadata;
    for (std::size_t k = 1; k < tracks.size(); ++k)
    {
        const auto *stream = r.demux->streams[tracks[k]];
        MediaAudioTrack track;
        track.pid = static_cast<std::uint32_t>(0x100 + k);
        track.audio_type = stream->disposition & AV_DISPOSITION_HEARING_IMPAIRED  ? 2
                           : stream->disposition & AV_DISPOSITION_VISUAL_IMPAIRED ? 3
                                                                                  : 0;
        if (const auto *language = av_dict_get(stream->metadata, "language", nullptr, 0))
            track.language.assign(language->value, strnlen(language->value, 31));
        if (const auto *title = av_dict_get(stream->metadata, "title", nullptr, 0))
            track.title.assign(title->value, strnlen(title->value, 127));
        else if (const auto *name = av_dict_get(stream->metadata, "comment", nullptr, 0))
            track.title.assign(name->value, strnlen(name->value, 127));
        audio_metadata.push_back(std::move(track));
    }
    if (output.subtitle_tracks)
        output.subtitle_tracks(output.context, subtitles);
    if (output.audio_tracks)
        output.audio_tracks(output.context, audio_metadata);
    r.packet = av_packet_alloc();
    if (!r.packet)
        return fail("Not enough memory to open this video.");
    int result = 0;
    auto video_clock = origin;
    bool first_video = true;
    struct Clock
    {
        std::int64_t last = AV_NOPTS_VALUE, next = AV_NOPTS_VALUE;
    };
    std::vector<Clock> clocks(mapping.size());
    std::int64_t discontinuity_offset = 0;
    struct PendingSubtitle
    {
        unsigned id;
        std::vector<std::uint8_t> data;
        std::int64_t pts, duration, offset;
    };
    std::vector<PendingSubtitle> pending_subtitles;
    std::size_t pending_subtitle_bytes = 0;
    const auto emit_subtitle = [&](unsigned id, const std::uint8_t *data, std::size_t bytes,
                                   std::int64_t pts, std::int64_t duration)
    {
        if (shift(pts, timestamp_offset))
            output.subtitle_packet(output.context, id, data, bytes, pts, duration);
    };
    const bool inline_seeks = input.take_seek && output.seek_commit && timeline.seekable;
    struct Armed
    {
        const MediaOutput &output;
        bool on = false;
        void set(bool armed)
        {
            if (on != armed && output.seek_armed)
                output.seek_armed(output.context, armed);
            on = armed;
        }
        ~Armed()
        {
            set(false);
        }
    } armed{output};
    std::string seek_error;
    const auto seek_inside = [&](std::int64_t requested)
    {
        const auto target = std::min(requested, timeline.duration_us - 1);
        const auto reopen = [&](const char *message)
        {
            if (r.cancelled())
                return 1;
            if (!output.seek_failed)
            {
                seek_error = message;
                return -1;
            }
            armed.set(false);
            output.seek_failed(output.context, target);
            return 1;
        };
        if (output.seek_begin)
            output.seek_begin(output.context);
        const char *message = "";
        const int sought = seek_input(target, &message);
        if (sought)
            return sought > 0 ? 1 : reopen(message);
        r.close_mux();
        if (!output.seek_commit(output.context, target))
            return reopen("The video could not continue at that position.");
        first_video = true;
        std::fill(clocks.begin(), clocks.end(), Clock{});
        discontinuity_offset = 0;
        pending_subtitles.clear();
        pending_subtitle_bytes = 0;
        video_clock = origin + target;
        if (const char *failed = open_mux())
            return reopen(failed);
        return 0;
    };
    armed.set(inline_seeks);
    while (!r.cancelled())
    {
        const bool gave_way = r.gave_way;
        if (gave_way)
        {
            r.gave_way = false;
            r.in->error = 0;
            r.in->eof_reached = 0;
        }
        if (inline_seeks)
        {
            const auto target = input.take_seek(input.context);
            if (target < 0 && gave_way)
                return 1;
            if (target >= 0)
            {
                const int sought = seek_inside(target);
                if (sought)
                    return sought > 0 ? 1 : fail(seek_error.c_str());
                if (r.cancelled())
                    break;
            }
        }
        r.reading = inline_seeks;
        result = av_read_frame(r.demux, r.packet);
        r.reading = false;
        if (r.gave_way)
        {
            av_packet_unref(r.packet);
            continue;
        }
        if (result < 0)
            break;
        const int source = r.packet->stream_index;
        if (source < 0 || static_cast<std::size_t>(source) >= mapping.size() ||
            r.packet->size < 0 || r.packet->size > 8 * 1024 * 1024)
            return fail("The video contains an invalid media packet.");
        if (hls)
        {
            const auto time_base = r.demux->streams[source]->time_base;
            if (time_base.num <= 0 || time_base.den <= 0)
                return fail("This video's timestamps are not supported.");
            const auto apply_offset = [&](std::int64_t us)
            {
                const auto ticks = av_rescale_q(us, AVRational{1, AV_TIME_BASE}, time_base);
                return ticks != INT64_MIN && shift(r.packet->dts, ticks) &&
                       shift(r.packet->pts, ticks);
            };
            if (!apply_offset(discontinuity_offset))
                return fail("This video's timestamps are not supported.");
            if (mapping[source] >= 0 && r.packet->dts != AV_NOPTS_VALUE)
            {
                auto dts = av_rescale_q(r.packet->dts, time_base, AVRational{1, AV_TIME_BASE});
                const auto duration =
                    av_rescale_q(r.packet->duration, time_base, AVRational{1, AV_TIME_BASE});
                constexpr auto bound = INT64_MAX / 4;
                if (dts < -bound || dts > bound || duration < 0 || duration > bound)
                    return fail("This video's timestamps are not supported.");
                const auto step = std::max(
                    duration, std::max<std::int64_t>(
                                  1, av_rescale_q(1, time_base, AVRational{1, AV_TIME_BASE})));
                auto &clock = clocks[source];
                // As in FFmpeg's input discontinuity correction, shift every
                // track by the same delta, preserving PTS/DTS reorder delay.
                // Repair provider resets before MPEG-TS rejects decreasing DTS.
                if (clock.next != AV_NOPTS_VALUE &&
                    (dts - clock.next > 10000000 || dts - clock.next < -10000000 ||
                     dts < clock.last - 100000))
                {
                    const auto correction = clock.next - dts;
                    if (!shift(discontinuity_offset, correction) || discontinuity_offset < -bound ||
                        discontinuity_offset > bound || !apply_offset(correction))
                        return fail("This video's timestamps are not supported.");
                    dts = clock.next;
                }
                // Encoder padding at a segment boundary can overlap by less
                // than a frame. Keep that track monotonic without moving every
                // other track's clock back and forth.
                if (clock.last != AV_NOPTS_VALUE && dts <= clock.last)
                {
                    if (!apply_offset(clock.next - dts))
                        return fail("This video's timestamps are not supported.");
                    dts = clock.next;
                }
                clock.last = dts;
                if (dts < -bound || step > bound || dts > bound - step)
                    return fail("This video's timestamps are not supported.");
                clock.next = dts + step;
            }
        }
        if (mapping[source] >= 0)
        {
            if (!first_packet_traced[source] && diag_enabled())
            {
                first_packet_traced[source] = 1;
                std::size_t skip_bytes = 0;
                const auto *skip =
                    av_packet_get_side_data(r.packet, AV_PKT_DATA_SKIP_SAMPLES, &skip_bytes);
                const unsigned skipped = skip && skip_bytes >= 4
                                             ? skip[0] | (unsigned)skip[1] << 8 |
                                                   (unsigned)skip[2] << 16 | (unsigned)skip[3] << 24
                                             : 0;
                const auto base = r.demux->streams[source]->time_base;
                char line[200];
                std::snprintf(line, sizeof(line),
                              "[player] media track %d first packet: pts %lld ms, dts %lld ms, "
                              "%u samples to skip",
                              source, to_ms(r.packet->pts, base), to_ms(r.packet->dts, base),
                              skipped);
                tv_diag_line(line);
            }
            if (source == video && first_video)
            {
                first_video = false;
                // In-band headers can reveal B-frame delay after the demuxer
                // assigned DTS = PTS to the first packet. Let the muxer infer
                // that initial decode time using the now-known reorder delay.
                if (v->video_delay > 0 && r.packet->pts != AV_NOPTS_VALUE &&
                    r.packet->dts == r.packet->pts)
                    r.packet->dts = AV_NOPTS_VALUE;
            }
            if (source == video && r.packet->pts != AV_NOPTS_VALUE)
            {
                video_clock = av_rescale_q(r.packet->pts, r.demux->streams[source]->time_base,
                                           AVRational{1, AV_TIME_BASE});
                if (video_clock < -INT64_MAX / 4 || video_clock > INT64_MAX / 4)
                    return fail("This video's timestamps are not supported.");
                for (auto it = pending_subtitles.begin(); it != pending_subtitles.end();)
                {
                    auto pts = it->pts;
                    if (shift(pts, discontinuity_offset - it->offset) &&
                        pts >= video_clock - 10000000)
                    {
                        emit_subtitle(it->id, it->data.data(), it->data.size(), pts, it->duration);
                        pending_subtitle_bytes -= it->data.size();
                        it = pending_subtitles.erase(it);
                    }
                    else
                        ++it;
                }
            }
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
            const auto bytes = static_cast<std::size_t>(r.packet->size);
            if (hls && base < video_clock - 10000000)
            {
                // HLS can deliver the next segment's WebVTT before the video
                // packet that reveals its reset clock. Hold it until that
                // correction is known, with bounded optional caption storage.
                // ponytail: 512 packets/1 MiB; raise only for measured caption bursts.
                if (pending_subtitles.size() < 512 && bytes <= Subtitles::max_packet &&
                    bytes <= kMaxWebVttBytes - pending_subtitle_bytes)
                {
                    pending_subtitles.push_back({static_cast<unsigned>(source) + 1,
                                                 {r.packet->data, r.packet->data + bytes},
                                                 base,
                                                 duration,
                                                 discontinuity_offset});
                    pending_subtitle_bytes += bytes;
                }
            }
            else
                emit_subtitle(static_cast<unsigned>(source) + 1, r.packet->data, bytes, base,
                              duration);
        }
        av_packet_unref(r.packet);
    }
    armed.set(false);
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
