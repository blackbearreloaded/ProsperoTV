// ProsperoTV - Bounded subtitle decoding with the pinned FFmpeg dependency.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "iptv_subtitles.h"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}
#include <algorithm>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <string_view>

namespace iptv
{
namespace
{
constexpr std::size_t kMaxText = 8192, kMaxExtra = 64 * 1024;
constexpr std::size_t kMaxCueBytes = 32 * 1024 * 1024, kMaxQueueBytes = 64 * 1024 * 1024;
constexpr std::int64_t kMaxDuration = 600000000;
AVCodecID codec_id(SubtitleCodec codec)
{
    switch (codec)
    {
    case SubtitleCodec::text:
        return AV_CODEC_ID_TEXT;
    case SubtitleCodec::subrip:
        return AV_CODEC_ID_SUBRIP;
    case SubtitleCodec::ass:
        return AV_CODEC_ID_ASS;
    case SubtitleCodec::mov_text:
        return AV_CODEC_ID_MOV_TEXT;
    case SubtitleCodec::webvtt:
        return AV_CODEC_ID_WEBVTT;
    case SubtitleCodec::dvb:
        return AV_CODEC_ID_DVB_SUBTITLE;
    case SubtitleCodec::dvd:
        return AV_CODEC_ID_DVD_SUBTITLE;
    case SubtitleCodec::pgs:
        return AV_CODEC_ID_HDMV_PGS_SUBTITLE;
    default:
        return AV_CODEC_ID_NONE;
    }
}
bool bitmap_codec(SubtitleCodec codec)
{
    return codec == SubtitleCodec::dvb || codec == SubtitleCodec::dvd ||
           codec == SubtitleCodec::pgs;
}
// FFmpeg's text decoders return ASS events. Preserve words, Unicode and line
// breaks without exposing formatting/drawing commands as visible dialogue.
std::string plain_ass(std::string_view input)
{
    const unsigned fields = input.starts_with("Dialogue:") ? 9 : 8;
    for (unsigned i = 0; i < fields; ++i)
    {
        const auto comma = input.find(',');
        if (comma == std::string_view::npos)
            return {};
        input.remove_prefix(comma + 1);
    }
    std::string out;
    bool drawing = false;
    for (std::size_t i = 0; i < input.size() && out.size() < kMaxText; ++i)
    {
        if (input[i] == '{')
        {
            const auto end = input.find('}', i + 1);
            if (end != std::string_view::npos)
            {
                for (auto j = i + 1; j + 2 < end; ++j)
                    if (input[j] == '\\' && input[j + 1] == 'p' && input[j + 2] >= '0' &&
                        input[j + 2] <= '9')
                        drawing = input[j + 2] != '0';
                i = end;
                continue;
            }
        }
        if (drawing)
            continue;
        if (input[i] == '\\' && i + 1 < input.size())
        {
            const char c = input[i + 1];
            if (c == 'N' || c == 'n' || c == 'h' || c == '\\' || c == '{' || c == '}')
            {
                out += c == 'N' || c == 'n' ? '\n' : c == 'h' ? ' ' : c;
                ++i;
                continue;
            }
        }
        if (static_cast<unsigned char>(input[i]) >= 32 || input[i] == '\n' || input[i] == '\t')
            out += input[i];
    }
    // A byte limit must not leave a partial UTF-8 code point at the end.
    if (out.size() == kMaxText)
    {
        while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xc0) == 0x80)
            out.pop_back();
        if (!out.empty() && static_cast<unsigned char>(out.back()) >= 0xc0)
            out.pop_back();
    }
    return out;
}
struct Decoded
{
    AVSubtitle value{};
    ~Decoded()
    {
        avsubtitle_free(&value);
    }
};
} // namespace

SubtitleCodec subtitle_codec(int id)
{
    for (const auto codec :
         {SubtitleCodec::text, SubtitleCodec::subrip, SubtitleCodec::ass, SubtitleCodec::mov_text,
          SubtitleCodec::webvtt, SubtitleCodec::dvb, SubtitleCodec::dvd, SubtitleCodec::pgs})
        if (codec_id(codec) == id)
            return codec;
    return SubtitleCodec::none;
}

struct Subtitles::State
{
    mutable std::mutex mutex;
    std::vector<SubtitleTrack> tracks;
    std::uint32_t selected = 0;
    SubtitleError error = SubtitleError::none;
    AVCodecContext *decoder = nullptr;
    AVPacket *packet = nullptr;
    struct Queued
    {
        std::shared_ptr<const SubtitleCue> cue;
        std::int64_t end;
        std::size_t bytes;
        bool replace;
    };
    std::vector<Queued> cues;
    std::size_t queued_bytes = 0;
    std::int64_t clock = -1;
    struct Packet
    {
        std::uint32_t id;
        std::vector<std::uint8_t> data;
        std::int64_t pts, duration;
    };
    std::deque<Packet> buffered;
    std::size_t buffered_bytes = 0;
    ~State()
    {
        close();
    }
    void close()
    {
        av_packet_free(&packet);
        avcodec_free_context(&decoder);
        cues.clear();
        queued_bytes = 0;
    }
    const SubtitleTrack *track() const
    {
        for (const auto &entry : tracks)
            if (entry.info.id == selected)
                return &entry;
        return nullptr;
    }
    bool open()
    {
        close();
        error = SubtitleError::none;
        if (!selected)
            return true;
        const auto *entry = track();
        const auto *codec = entry ? avcodec_find_decoder(codec_id(entry->info.codec)) : nullptr;
        if (codec)
            decoder = avcodec_alloc_context3(codec);
        if (!decoder)
        {
            error = SubtitleError::unavailable;
            return false;
        }
        decoder->pkt_timebase = AVRational{1, AV_TIME_BASE};
        decoder->width = static_cast<int>(entry->width);
        decoder->height = static_cast<int>(entry->height);
        decoder->max_pixels = 3840 * 2160;
        decoder->thread_count = 1;
        if (!entry->extra.empty())
        {
            decoder->extradata = static_cast<std::uint8_t *>(
                av_mallocz(entry->extra.size() + AV_INPUT_BUFFER_PADDING_SIZE));
            if (decoder->extradata)
            {
                std::memcpy(decoder->extradata, entry->extra.data(), entry->extra.size());
                decoder->extradata_size = static_cast<int>(entry->extra.size());
            }
        }
        packet = av_packet_alloc();
        if ((!entry->extra.empty() && !decoder->extradata) || !packet ||
            avcodec_open2(decoder, codec, nullptr) < 0)
        {
            close();
            error = SubtitleError::unavailable;
            return false;
        }
        return true;
    }
    void expire(bool packets = true)
    {
        for (auto it = cues.begin(); it != cues.end();)
            if (it->end <= clock)
            {
                queued_bytes -= it->bytes;
                it = cues.erase(it);
            }
            else
                ++it;
        // Retain a short history for bitmap composition and cues that span a
        // language change. Downloads can run ahead while subtitles are Off.
        while (packets && !buffered.empty() && buffered.front().pts < clock - 60000000)
        {
            buffered_bytes -= buffered.front().data.size();
            buffered.pop_front();
        }
    }
};

Subtitles::Subtitles() : state_(std::make_unique<State>())
{
}
Subtitles::~Subtitles() = default;
void Subtitles::set_tracks(std::vector<SubtitleTrack> tracks)
{
    std::lock_guard lock(state_->mutex);
    auto &s = *state_;
    std::vector<SubtitleTrack> accepted;
    for (auto &track : tracks)
    {
        if (!track.info.id || track.info.codec == SubtitleCodec::none ||
            track.extra.size() > kMaxExtra || track.width > 3840 || track.height > 2160 ||
            track.info.language.size() > 32 || track.info.title.size() > 128 ||
            accepted.size() >= max_tracks)
            continue;
        if (std::any_of(accepted.begin(), accepted.end(),
                        [&](const auto &prior) { return prior.info.id == track.info.id; }))
            continue;
        accepted.push_back(std::move(track));
    }
    const auto same_format = [](const SubtitleTrack &a, const SubtitleTrack &b)
    {
        return a.info.codec == b.info.codec && a.extra == b.extra && a.width == b.width &&
               a.height == b.height;
    };
    for (auto it = s.buffered.begin(); it != s.buffered.end();)
    {
        const auto previous = std::find_if(s.tracks.begin(), s.tracks.end(), [&](const auto &track)
                                           { return track.info.id == it->id; });
        const auto current = std::find_if(accepted.begin(), accepted.end(), [&](const auto &track)
                                          { return track.info.id == it->id; });
        if (previous == s.tracks.end() || current == accepted.end() ||
            !same_format(*previous, *current))
        {
            s.buffered_bytes -= it->data.size();
            it = s.buffered.erase(it);
        }
        else
            ++it;
    }
    const auto previous = s.track() ? *s.track() : SubtitleTrack{};
    s.tracks = std::move(accepted);
    const auto *current = s.track();
    if (s.selected && !current)
    {
        s.selected = 0;
        s.open();
    }
    else if (current && !same_format(*current, previous))
        s.open();
}
SubtitleState Subtitles::state() const
{
    std::lock_guard lock(state_->mutex);
    SubtitleState out;
    out.selected = state_->selected;
    out.error = state_->error;
    for (const auto &track : state_->tracks)
        out.tracks.push_back(track.info);
    return out;
}
bool Subtitles::select(std::uint32_t id)
{
    std::lock_guard lock(state_->mutex);
    auto &s = *state_;
    if (id && std::none_of(s.tracks.begin(), s.tracks.end(),
                           [&](const auto &track) { return track.info.id == id; }))
        return false;
    if (id == s.selected && s.error == SubtitleError::none)
        return true;
    s.selected = id;
    if (!s.open())
        return false;
    if (id)
        for (const auto &packet : s.buffered)
            if (packet.id == id)
                (void)decode_locked(id, packet.data.data(), packet.data.size(), packet.pts,
                                    packet.duration);
    return true;
}
void Subtitles::reset_timeline()
{
    std::lock_guard lock(state_->mutex);
    // Reopen to clear bitmap display state as well as ordinary codec buffers.
    state_->buffered.clear();
    state_->buffered_bytes = 0;
    state_->clock = -1;
    state_->open();
}
void Subtitles::clear()
{
    std::lock_guard lock(state_->mutex);
    state_->tracks.clear();
    state_->selected = 0;
    state_->buffered.clear();
    state_->buffered_bytes = 0;
    state_->clock = -1;
    state_->open();
}
bool Subtitles::push(std::uint32_t id, const std::uint8_t *data, std::size_t bytes,
                     std::int64_t pts_us, std::int64_t duration_us)
{
    std::lock_guard lock(state_->mutex);
    auto &s = *state_;
    const auto track = std::find_if(s.tracks.begin(), s.tracks.end(),
                                    [&](const auto &item) { return item.info.id == id; });
    if (track == s.tracks.end())
        return true;
    if (!data || !bytes || bytes > max_packet)
    {
        s.error = SubtitleError::limit;
        return false;
    }
    if (pts_us < 0 || pts_us > std::numeric_limits<std::int64_t>::max() - kMaxDuration)
        return true;
    // HLS repeats a WebVTT cue in every segment it overlaps. Cache/decode it once,
    // including while captions are Off, so language changes cannot double it.
    if (track->info.codec == SubtitleCodec::webvtt &&
        std::any_of(s.buffered.begin(), s.buffered.end(),
                    [&](const auto &packet)
                    {
                        return packet.id == id && packet.pts == pts_us &&
                               packet.duration == duration_us && packet.data.size() == bytes &&
                               std::memcmp(packet.data.data(), data, bytes) == 0;
                    }))
        return true;
    while (!s.buffered.empty() &&
           (s.buffered.size() >= 2048 || s.buffered_bytes + bytes > 16 * 1024 * 1024))
    {
        s.buffered_bytes -= s.buffered.front().data.size();
        s.buffered.pop_front();
    }
    s.buffered.push_back({id, {data, data + bytes}, pts_us, duration_us});
    s.buffered_bytes += bytes;
    return decode_locked(id, data, bytes, pts_us, duration_us);
}
bool Subtitles::decode_locked(std::uint32_t id, const std::uint8_t *data, std::size_t bytes,
                              std::int64_t pts_us, std::int64_t duration_us)
{
    auto &s = *state_;
    if (!id || id != s.selected)
        return true;
    if (!s.decoder)
        return false;
    const auto fail = [&](SubtitleError error)
    {
        s.error = error;
        return false;
    };
    if (!data || !bytes || bytes > max_packet)
        return fail(SubtitleError::limit);
    if (pts_us < 0 || pts_us > std::numeric_limits<std::int64_t>::max() - kMaxDuration)
        return true; // An untimed cue cannot be aligned with the picture.
    av_packet_unref(s.packet);
    if (av_new_packet(s.packet, static_cast<int>(bytes)) < 0)
        return fail(SubtitleError::limit);
    std::memcpy(s.packet->data, data, bytes);
    s.packet->pts = pts_us;
    s.packet->duration = std::clamp(duration_us, INT64_C(0), kMaxDuration);
    Decoded decoded;
    int got = 0;
    const int result = avcodec_decode_subtitle2(s.decoder, &decoded.value, &got, s.packet);
    if (result < 0)
        return fail(SubtitleError::malformed);
    if (!got)
        return true;
    const auto &sub = decoded.value;
    if (sub.num_rects > 32 || sub.start_display_time > kMaxDuration / 1000)
        return fail(SubtitleError::limit);
    auto cue = std::make_shared<SubtitleCue>();
    const auto base = sub.pts == AV_NOPTS_VALUE ? pts_us : sub.pts;
    if (base < 0 || base > std::numeric_limits<std::int64_t>::max() - kMaxDuration)
        return fail(SubtitleError::malformed);
    cue->start_us = base + sub.start_display_time * INT64_C(1000);
    const auto duration =
        sub.end_display_time > sub.start_display_time
            ? std::min<std::int64_t>(
                  (sub.end_display_time - sub.start_display_time) * INT64_C(1000), kMaxDuration)
        : duration_us > 0 ? std::min(duration_us, kMaxDuration)
                          : INT64_C(5000000);
    if (cue->start_us > std::numeric_limits<std::int64_t>::max() - duration)
        return fail(SubtitleError::malformed);
    cue->end_us = cue->start_us + duration;
    cue->canvas_width = static_cast<unsigned>(std::max(0, s.decoder->width));
    cue->canvas_height = static_cast<unsigned>(std::max(0, s.decoder->height));
    std::size_t cue_bytes = sizeof(SubtitleCue);
    for (unsigned i = 0; i < sub.num_rects; ++i)
    {
        if (!sub.rects || !sub.rects[i])
            return fail(SubtitleError::malformed);
        const auto &rect = *sub.rects[i];
        if (rect.type == SUBTITLE_ASS || rect.type == SUBTITLE_TEXT)
        {
            const char *text = rect.type == SUBTITLE_ASS ? rect.ass : rect.text;
            if (!text || strnlen(text, max_packet + 1) > max_packet)
                return fail(SubtitleError::limit);
            const auto line = rect.type == SUBTITLE_ASS ? plain_ass(text) : std::string(text);
            if (cue->text.size() + line.size() + 1 > kMaxText)
                return fail(SubtitleError::limit);
            if (!cue->text.empty() && !line.empty())
                cue->text += '\n';
            cue->text += line;
        }
        else if (rect.type == SUBTITLE_BITMAP)
        {
            if (rect.w <= 0 || rect.h <= 0 || rect.w > 3840 || rect.h > 2160 || rect.x < -3840 ||
                rect.y < -2160 || rect.x > 3840 || rect.y > 2160 || rect.linesize[0] < rect.w ||
                !rect.data[0] || !rect.data[1] || rect.nb_colors < 1 || rect.nb_colors > 256 ||
                cue->canvas_width > 3840 || cue->canvas_height > 2160)
                return fail(SubtitleError::malformed);
            const auto pixels = static_cast<std::size_t>(rect.w) * rect.h;
            if (pixels * 4 > kMaxCueBytes - cue_bytes)
                return fail(SubtitleError::limit);
            SubtitleBitmap bitmap;
            bitmap.x = rect.x;
            bitmap.y = rect.y;
            bitmap.width = rect.w;
            bitmap.height = rect.h;
            bitmap.argb.resize(pixels);
            for (int y = 0; y < rect.h; ++y)
                for (int x = 0; x < rect.w; ++x)
                {
                    const auto color =
                        rect.data[0][static_cast<std::size_t>(y) * rect.linesize[0] + x];
                    if (color >= rect.nb_colors)
                        return fail(SubtitleError::malformed);
                    std::memcpy(&bitmap.argb[static_cast<std::size_t>(y) * rect.w + x],
                                rect.data[1] + color * 4, 4);
                }
            cue_bytes += pixels * 4;
            cue->bitmaps.push_back(std::move(bitmap));
        }
    }
    cue_bytes += cue->text.size();
    s.expire(false);
    if (s.cues.size() >= 512 || cue_bytes > kMaxQueueBytes - s.queued_bytes)
        return fail(SubtitleError::limit);
    const bool replace = bitmap_codec(s.track()->info.codec);
    std::int64_t end = cue->end_us;
    if (replace)
        for (auto &prior : s.cues)
            if (prior.replace)
            {
                if (prior.cue->start_us <= cue->start_us)
                    prior.end = std::min(prior.end, cue->start_us);
                else
                    end = std::min(end, prior.cue->start_us);
            }
    s.cues.push_back({std::move(cue), end, cue_bytes, replace});
    s.queued_bytes += cue_bytes;
    s.error = SubtitleError::none;
    return true;
}
std::vector<std::shared_ptr<const SubtitleCue>> Subtitles::at(std::int64_t pts)
{
    std::lock_guard lock(state_->mutex);
    auto &s = *state_;
    std::vector<std::shared_ptr<const SubtitleCue>> active;
    if (pts < 0)
        return active;
    s.clock = pts;
    s.expire();
    for (const auto &item : s.cues)
        if (item.cue->start_us <= pts && pts < item.end &&
            (!item.cue->text.empty() || !item.cue->bitmaps.empty()))
        {
            if (active.size() == 8)
                active.erase(active.begin());
            active.push_back(item.cue);
        }
    return active;
}
} // namespace iptv
