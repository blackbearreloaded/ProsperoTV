/* psiptv - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Native PS5 IPTV decode/audio backend. Transport and UI independent. */

#include "iptv_native_backend.h"
#include "iptv_native_agc_present.h"
#include "iptv_vp9_packet.h"

#ifdef IPTV_NATIVE_BACKEND_STATE_TEST
#include <assert.h>
#endif
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define BACKEND_MAGIC UINT32_C(0x49505456)
#define PIPELINE_BUFFER_COUNT 3u
#define PENDING_PTS_CAPACITY 8u
#define VIDEO_DRAIN_FLUSH_LIMIT PENDING_PTS_CAPACITY
#define INPUT_SLOT_BYTES 0x800000u
#define AUDIODEC_AAC 3u
#define AUDIODEC_WORD_S16 1
#define AUDIO_OUT_GRAIN 256u
#define AUDIO_OUT_RATE 48000u
#define AUDIO_OUT_STEREO_S16 1u
#define AUDIO_OUT_VOLUME_0DB 0x8000
#define AUDIO_PCM_BYTES 0x4000u
#define AUDIO_FRAME_MAX_BYTES 4608u
#define VIDEO_MODULE_ID 207u
#define AUDIO_MODULE_ID 0x0088u
#define PACE_SLEEP_SLICE_US 5000u
#define PACE_MAX_WAIT_US UINT64_C(500000)
#define PACE_DISCONTINUITY_US UINT64_C(2000000)
#define PACE_BACKWARD_TOLERANCE_US UINT64_C(100000)

#define IPTV_NATIVE_E_ARGUMENT (-1000)
#define IPTV_NATIVE_E_STATE (-1001)
#define IPTV_NATIVE_E_UNSUPPORTED (-1002)
#define IPTV_NATIVE_E_ACCESS_UNIT (-1003)
#define IPTV_NATIVE_E_DECODER_OUTPUT (-1004)
#define IPTV_NATIVE_E_AUDIO_FRAME (-1005)
#define IPTV_NATIVE_E_CANCELLED (-125)

typedef struct videodec2_decoder_config
{
    uint64_t size;
    uint32_t resource_type, codec_type, profile, max_level;
    int32_t max_width, max_height, max_dpb_frames;
    uint32_t pipeline_depth;
    uint64_t compute_queue, cpu_affinity;
    int32_t cpu_priority;
    uint32_t optimize_progressive, check_memory_type, reserved;
} videodec2_decoder_config_t;

typedef struct videodec2_decoder_memory
{
    uint64_t size, cpu_size;
    void *cpu;
    uint64_t gpu_size;
    void *gpu;
    uint64_t cpu_gpu_size;
    void *cpu_gpu;
    uint64_t max_frame_size;
    uint32_t frame_alignment, reserved;
} videodec2_decoder_memory_t;

typedef struct videodec2_compute_config
{
    uint64_t size;
    uint16_t pipe_id, queue_id;
    uint8_t check_memory_type, reserved0;
    uint16_t reserved1;
} videodec2_compute_config_t;

typedef struct videodec2_compute_memory
{
    uint64_t size, cpu_gpu_size;
    void *cpu_gpu;
} videodec2_compute_memory_t;

typedef struct videodec2_input
{
    uint64_t size;
    void *au;
    uint64_t au_size, pts, dts, attached;
} videodec2_input_t;

typedef struct videodec2_frame
{
    uint64_t size;
    void *buffer;
    uint64_t buffer_size;
    uint32_t accepted, reserved;
} videodec2_frame_t;

typedef struct videodec2_output
{
    uint64_t size;
    uint8_t valid, error, picture_count, padding;
    uint32_t codec, width, pitch, height, reserved;
    void *buffer;
    uint64_t buffer_size;
    uint32_t frame_format, pitch_bytes;
} videodec2_output_t;

typedef struct sce_audiodec_au_info
{
    uint32_t size;
    void *address;
    uint32_t length;
} sce_audiodec_au_info_t;

typedef struct sce_audiodec_pcm_item
{
    uint32_t size;
    void *address;
    uint32_t length;
} sce_audiodec_pcm_item_t;

typedef struct sce_audiodec_ctrl
{
    void *param;
    void *stream_info;
    sce_audiodec_au_info_t *au_info;
    sce_audiodec_pcm_item_t *pcm_item;
} sce_audiodec_ctrl_t;

typedef struct sce_audiodec_param_aac
{
    uint32_t size;
    int32_t word_size;
    uint32_t config_number;
    uint32_t sampling_frequency_index;
    uint32_t max_channels;
    uint32_t enable_he_aac;
} sce_audiodec_param_aac_t;

typedef struct sce_audiodec_aac_info
{
    uint32_t size;
    uint32_t sampling_frequency;
    uint32_t channel_count;
    uint32_t he_aac;
    int32_t result;
} sce_audiodec_aac_info_t;

typedef struct native_video_mode
{
    iptv_native_codec_t codec;
    uint32_t accepted_profile;
    uint32_t decoder_codec;
    uint32_t decoder_profile;
    uint32_t max_level;
    uint32_t coded_width;
    uint32_t coded_height;
    uint32_t decoder_max_width;
    uint32_t decoder_max_height;
    uint32_t output_width;
    uint32_t output_height;
    uint32_t alternate_output_height;
    uint32_t output_pitch;
    uint32_t visible_width;
    uint32_t visible_height;
} native_video_mode_t;

static const native_video_mode_t video_modes[] = {
    {IPTV_NATIVE_CODEC_H264, IPTV_NATIVE_H264_PROFILE_BASELINE, 1,
     IPTV_NATIVE_H264_PROFILE_BASELINE, 41, 1280, 720, 1280, 720, 1280, 720, 0, 1280, 1280, 720},
    {IPTV_NATIVE_CODEC_H264, IPTV_NATIVE_H264_PROFILE_MAIN, 1, IPTV_NATIVE_H264_PROFILE_MAIN, 41,
     1280, 720, 1280, 720, 1280, 720, 0, 1280, 1280, 720},
    {IPTV_NATIVE_CODEC_H264, IPTV_NATIVE_H264_PROFILE_HIGH, 1, IPTV_NATIVE_H264_PROFILE_HIGH, 41,
     1280, 720, 1280, 720, 1280, 720, 0, 1280, 1280, 720},
    {IPTV_NATIVE_CODEC_HEVC_MAIN8, IPTV_NATIVE_HEVC_PROFILE_MAIN, 0x000ee049,
     IPTV_NATIVE_HEVC_PROFILE_MAIN, 123, 1280, 720, 1280, 720, 1280, 720, 0, 1280, 1280, 720},
    {IPTV_NATIVE_CODEC_H264, 0, 1, 100, 51, 1920, 1088, 1920, 1088, 1920, 1088, 0, 2048, 1920,
     1080},
    {IPTV_NATIVE_CODEC_H264, 0, 1, 100, 51, 2560, 1440, 2560, 1440, 2560, 1440, 0, 2560, 2560,
     1440},
    {IPTV_NATIVE_CODEC_H264, 0, 1, 100, 52, 3840, 2160, 3840, 2176, 3840, 2160, 2176, 3840, 3840,
     2160},
    {IPTV_NATIVE_CODEC_H264, 0, 1, 100, 52, 3840, 2176, 3840, 2176, 3840, 2160, 2176, 3840, 3840,
     2160},
    {IPTV_NATIVE_CODEC_HEVC_MAIN8, IPTV_NATIVE_HEVC_PROFILE_MAIN, 0x000ee049, 1, 123, 1920, 1080,
     1920, 1088, 1920, 1088, 0, 2048, 1920, 1080},
    {IPTV_NATIVE_CODEC_HEVC_MAIN8, IPTV_NATIVE_HEVC_PROFILE_MAIN, 0x000ee049, 1, 123, 1920, 1088,
     1920, 1088, 1920, 1088, 0, 2048, 1920, 1080},
    {IPTV_NATIVE_CODEC_HEVC_MAIN8, IPTV_NATIVE_HEVC_PROFILE_MAIN, 0x000ee049, 1, 150, 2560, 1440,
     2560, 1440, 2560, 1440, 0, 2560, 2560, 1440},
    {IPTV_NATIVE_CODEC_HEVC_MAIN8, IPTV_NATIVE_HEVC_PROFILE_MAIN, 0x000ee049, 1, 153, 3840, 2160,
     3840, 2176, 3840, 2160, 2176, 3840, 3840, 2160},
    {IPTV_NATIVE_CODEC_HEVC_MAIN8, IPTV_NATIVE_HEVC_PROFILE_MAIN, 0x000ee049, 1, 153, 3840, 2176,
     3840, 2176, 3840, 2160, 2176, 3840, 3840, 2160},
    {IPTV_NATIVE_CODEC_VP9_PROFILE0, IPTV_NATIVE_VP9_PROFILE_0, 0x00245bfd,
     IPTV_NATIVE_VP9_PROFILE_0, 41, 1920, 1080, 1920, 1080, 1920, 1080, 0, 2048, 1920, 1080},
    {IPTV_NATIVE_CODEC_VP9_PROFILE0, IPTV_NATIVE_VP9_PROFILE_0, 0x00245bfd,
     IPTV_NATIVE_VP9_PROFILE_0, 50, 2560, 1440, 2560, 1440, 2560, 1440, 0, 2560, 2560, 1440},
    {IPTV_NATIVE_CODEC_VP9_PROFILE0, IPTV_NATIVE_VP9_PROFILE_0, 0x00245bfd,
     IPTV_NATIVE_VP9_PROFILE_0, 51, 3840, 2160, 3840, 2160, 3840, 2160, 0, 3840, 3840, 2160},
};

typedef struct direct_allocation
{
    void *address;
    int64_t start;
    size_t size;
} direct_allocation_t;

typedef struct pending_pts
{
    struct
    {
        uint64_t pts_us;
        uint8_t displayable;
    } values[PENDING_PTS_CAPACITY];
    uint32_t count;
} pending_pts_t;

_Static_assert(VIDEO_DRAIN_FLUSH_LIMIT >= PENDING_PTS_CAPACITY,
               "bounded drain must cover every retained timestamp");

typedef struct audio_sink
{
    int32_t handle;
    uint32_t input_rate;
    uint32_t channels;
    uint64_t input_index;
    uint64_t next_output_position;
    int16_t previous_left;
    int16_t previous_right;
    uint32_t pending;
    uint8_t have_previous;
    uint8_t drained;
    int16_t block[AUDIO_OUT_GRAIN * 2u];
} audio_sink_t;

typedef struct backend_state
{
    uint32_t magic;
    iptv_native_state_t state;
    _Atomic int stop_requested;
    const native_video_mode_t *mode;
    iptv_native_open_config_t config;
    iptv_native_telemetry_t telemetry;

    void *decoder;
    void *compute_queue;
    videodec2_decoder_memory_t decoder_memory;
    videodec2_compute_memory_t compute_memory;
    direct_allocation_t compute_allocation;
    direct_allocation_t gpu_allocation;
    direct_allocation_t cpu_gpu_allocation;
    direct_allocation_t input_allocation;
    direct_allocation_t frame_allocation;
    size_t cpu_mapping_size;
    size_t input_slot_size;
    size_t frame_slot_size;
    uint32_t video_module_loaded;

    int32_t audio_decoder;
    uint32_t audio_module_loaded;
    uint32_t audio_library_initialized;
    sce_audiodec_param_aac_t audio_param;
    sce_audiodec_aac_info_t audio_info;
    sce_audiodec_au_info_t audio_au;
    sce_audiodec_pcm_item_t audio_pcm_item;
    sce_audiodec_ctrl_t audio_ctrl;
    audio_sink_t audio_sink;
    uint8_t audio_pcm[AUDIO_PCM_BYTES];

    uint64_t open_started_us;
    uint64_t pace_base_pts_us;
    uint64_t pace_base_clock_us;
    uint64_t pace_last_pts_us;
    pending_pts_t pending_pts;
    uint8_t drain_started;
    uint8_t video_drained;
    uint8_t pace_active;
} backend_state_t;

_Static_assert(sizeof(backend_state_t) <= IPTV_NATIVE_BACKEND_STORAGE_BYTES,
               "public backend storage must contain the native state");
_Static_assert(_Alignof(backend_state_t) <= _Alignof(iptv_native_backend_t),
               "public backend storage must preserve native alignment");

int sceKernelUsleep(uint32_t microseconds);
int64_t sceKernelGetDirectMemorySize(void);
int32_t sceKernelAllocateDirectMemory(int64_t search_start, int64_t search_end, size_t length,
                                      size_t alignment, int memory_type,
                                      int64_t *direct_memory_start);
int32_t sceKernelMapDirectMemory(void **address, size_t length, int protection, int flags,
                                 int64_t direct_memory_start, size_t alignment);
int32_t sceKernelAvailableFlexibleMemorySize(size_t *out_size);
int32_t sceKernelMapNamedFlexibleMemory(void **address, size_t length, int protection, int flags,
                                        const char *name);
int32_t sceKernelReleaseFlexibleMemory(void *address, size_t length);
int32_t sceKernelVirtualQuery(void *address, int flags, void *info, size_t info_size);
int32_t sceKernelMunmap(void *address, size_t length);
int32_t sceKernelReleaseDirectMemory(int64_t direct_memory_start, size_t length);
int32_t sceSysmoduleLoadModule(uint32_t id);
int32_t sceSysmoduleUnloadModule(uint32_t id);

int32_t sceVideodec2QueryDecoderMemoryInfo(const videodec2_decoder_config_t *config,
                                           videodec2_decoder_memory_t *memory);
int32_t sceVideodec2QueryComputeMemoryInfo(videodec2_compute_memory_t *memory);
int32_t sceVideodec2AllocateComputeQueue(const videodec2_compute_config_t *config,
                                         const videodec2_compute_memory_t *memory, void **queue);
int32_t sceVideodec2ReleaseComputeQueue(void *queue);
int32_t sceVideodec2CreateDecoder(const videodec2_decoder_config_t *config,
                                  const videodec2_decoder_memory_t *memory, void **decoder);
int32_t sceVideodec2DeleteDecoder(void *decoder);
int32_t sceVideodec2Reset(void *decoder);
int32_t sceVideodec2Decode(void *decoder, videodec2_input_t *input, videodec2_frame_t *frame,
                           videodec2_output_t *output);
int32_t sceVideodec2Flush(void *decoder, videodec2_frame_t *frame, videodec2_output_t *output);

int sceAudiodecInitLibrary(uint32_t codec_type);
int sceAudiodecTermLibrary(uint32_t codec_type);
int sceAudiodecCreateDecoder(sce_audiodec_ctrl_t *ctrl, uint32_t codec_type);
int sceAudiodecDeleteDecoder(int handle);
int sceAudiodecDecode(int handle, sce_audiodec_ctrl_t *ctrl);
int sceAudioOutInit(void);
int sceAudioOutOpen(int user_id, int type, int index, uint32_t length, uint32_t frequency,
                    uint32_t format);
int sceAudioOutClose(int handle);
int sceAudioOutOutput(int handle, const void *samples);
int sceAudioOutSetVolume(int handle, int flags, const int *volumes);

static backend_state_t *state_from(iptv_native_backend_t *backend)
{
    return backend ? (backend_state_t *)(void *)backend->storage : NULL;
}

static const backend_state_t *const_state_from(const iptv_native_backend_t *backend)
{
    return backend ? (const backend_state_t *)(const void *)backend->storage : NULL;
}

static size_t align_16k(size_t value)
{
    return (value + 0x3fffu) & ~(size_t)0x3fffu;
}

static uint64_t monotonic_us(void)
{
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * UINT64_C(1000000) + (uint64_t)now.tv_nsec / UINT64_C(1000);
}

static void reset_allocation(direct_allocation_t *allocation)
{
    allocation->address = NULL;
    allocation->start = -1;
    allocation->size = 0;
}

static int32_t allocate_direct(direct_allocation_t *allocation, size_t size, int protection,
                               int64_t limit)
{
    int32_t result;

    if (!allocation || size == 0 || limit <= 0)
        return IPTV_NATIVE_E_ARGUMENT;
    allocation->size = size;
    result = sceKernelAllocateDirectMemory(0, limit, size, 0x4000, 12, &allocation->start);
    if (result == 0)
        result = sceKernelMapDirectMemory(&allocation->address, size, protection, 0,
                                          allocation->start, 0x4000);
    return result;
}

static int32_t release_direct(direct_allocation_t *allocation)
{
    int32_t first_result = 0;
    int32_t result;

    if (allocation->address)
    {
        result = sceKernelMunmap(allocation->address, allocation->size);
        if (result != 0)
            first_result = result;
    }
    if (allocation->start >= 0)
    {
        result = sceKernelReleaseDirectMemory(allocation->start, allocation->size);
        if (first_result == 0 && result != 0)
            first_result = result;
    }
    reset_allocation(allocation);
    return first_result;
}

static const native_video_mode_t *find_video_mode(const iptv_native_open_config_t *config)
{
    size_t index;

    for (index = 0; index < sizeof(video_modes) / sizeof(video_modes[0]); ++index)
    {
        const native_video_mode_t *mode = &video_modes[index];
        if (mode->codec == config->codec &&
            (mode->accepted_profile == 0 || mode->accepted_profile == config->profile) &&
            mode->coded_width == config->coded_width &&
            mode->coded_height == config->coded_height &&
            mode->visible_width == config->visible_width &&
            mode->visible_height == config->visible_height)
            return mode;
    }
    return NULL;
}

static int profile_supported(const iptv_native_open_config_t *config,
                             const native_video_mode_t *mode)
{
    if (config->level == 0 || config->level > mode->max_level)
        return 0;
    if (config->codec == IPTV_NATIVE_CODEC_H264)
        return config->profile == 66 || config->profile == 77 || config->profile == 100;
    if (config->codec == IPTV_NATIVE_CODEC_HEVC_MAIN8)
        return config->profile == IPTV_NATIVE_HEVC_PROFILE_MAIN;
    return config->codec == IPTV_NATIVE_CODEC_VP9_PROFILE0 &&
           config->profile == IPTV_NATIVE_VP9_PROFILE_0;
}

static int pending_pts_push(pending_pts_t *pending, uint64_t pts_us, int displayable)
{
    if (pending->count == PENDING_PTS_CAPACITY)
        return 0;
    pending->values[pending->count].pts_us = pts_us;
    pending->values[pending->count].displayable = displayable != 0;
    ++pending->count;
    return 1;
}

static int pending_pts_take_smallest(pending_pts_t *pending, uint64_t *pts_us, int *displayable)
{
    uint32_t smallest;
    uint32_t index;

    if (!pending || !pts_us || !displayable || pending->count == 0)
        return 0;
    smallest = 0;
    for (index = 1; index < pending->count; ++index)
    {
        uint64_t candidate = pending->values[index].pts_us;
        uint64_t selected = pending->values[smallest].pts_us;
        if ((selected == UINT64_MAX && candidate != UINT64_MAX) ||
            (candidate != UINT64_MAX && candidate < selected))
            smallest = index;
    }
    *pts_us = pending->values[smallest].pts_us;
    *displayable = pending->values[smallest].displayable;
    --pending->count;
    pending->values[smallest] = pending->values[pending->count];
    return 1;
}

static int pending_pts_take_first(pending_pts_t *pending, uint64_t *pts_us, int *displayable)
{
    uint32_t index;

    if (!pending || !pts_us || !displayable || pending->count == 0)
        return 0;
    *pts_us = pending->values[0].pts_us;
    *displayable = pending->values[0].displayable;
    --pending->count;
    for (index = 0; index < pending->count; ++index)
        pending->values[index] = pending->values[index + 1u];
    return 1;
}

static int state_pending_push(backend_state_t *state, uint64_t pts_us, int displayable)
{
    if (!pending_pts_push(&state->pending_pts, pts_us, displayable))
        return 0;
    if (pts_us == UINT64_MAX)
        ++state->telemetry.unknown_video_timestamps;
    state->telemetry.pending_video_timestamps = state->pending_pts.count;
    return 1;
}

static int state_pending_take(backend_state_t *state, uint64_t *pts_us, int *displayable)
{
    int result = state->config.codec == IPTV_NATIVE_CODEC_VP9_PROFILE0
                     ? pending_pts_take_first(&state->pending_pts, pts_us, displayable)
                     : pending_pts_take_smallest(&state->pending_pts, pts_us, displayable);
    state->telemetry.pending_video_timestamps = state->pending_pts.count;
    return result;
}

static void discard_pending_video(backend_state_t *state)
{
    state->telemetry.dropped_delayed_frames += state->pending_pts.count;
    state->pending_pts.count = 0;
    state->telemetry.pending_video_timestamps = 0;
}

static int frame_is_in_pool(const backend_state_t *state, const void *frame)
{
    uint32_t index;

    for (index = 0; index < PIPELINE_BUFFER_COUNT; ++index)
    {
        if (frame ==
            (const uint8_t *)state->frame_allocation.address + index * state->frame_slot_size)
            return 1;
    }
    return 0;
}

static int annex_b_has_vcl(iptv_native_codec_t codec, const uint8_t *data, size_t bytes)
{
    size_t index = 0;

    while (index + 4 < bytes)
    {
        size_t prefix = 0;
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1)
            prefix = 3;
        else if (index + 4 < bytes && data[index] == 0 && data[index + 1] == 0 &&
                 data[index + 2] == 0 && data[index + 3] == 1)
            prefix = 4;
        if (prefix != 0)
        {
            uint8_t header = data[index + prefix];
            if (codec == IPTV_NATIVE_CODEC_H264)
            {
                uint8_t type = header & 0x1fu;
                if (type >= 1 && type <= 5)
                    return 1;
            }
            else
            {
                uint8_t type = (header >> 1) & 0x3fu;
                if (type <= 31)
                    return 1;
            }
            index += prefix;
        }
        else
        {
            ++index;
        }
    }
    return 0;
}

static uint32_t adts_core_rate(const uint8_t *adts, size_t bytes)
{
    static const uint32_t rates[] = {96000u, 88200u, 64000u, 48000u, 44100u, 32000u,
                                     24000u, 22050u, 16000u, 12000u, 11025u, 8000u};
    uint32_t index;

    if (!adts || bytes < 7)
        return 0;
    index = (adts[2] >> 2) & 0x0fu;
    return index < sizeof(rates) / sizeof(rates[0]) ? rates[index] : 0;
}

static uint32_t adts_channels(const uint8_t *adts, size_t bytes)
{
    if (!adts || bytes < 7)
        return 0;
    return ((uint32_t)(adts[2] & 1u) << 2) | ((uint32_t)(adts[3] >> 6) & 3u);
}

static uint32_t decoded_pcm_rate(const uint8_t *adts, size_t bytes, uint32_t channels,
                                 uint32_t pcm_bytes, uint32_t fallback_rate)
{
    uint32_t core_rate = adts_core_rate(adts, bytes);
    uint32_t blocks;
    uint32_t frames;
    uint32_t coded_frames;
    uint64_t rate;

    if (core_rate == 0 || channels == 0)
        return fallback_rate;
    blocks = (adts[6] & 3u) + 1u;
    frames = pcm_bytes / (sizeof(int16_t) * channels);
    coded_frames = 1024u * blocks;
    rate = ((uint64_t)core_rate * frames + coded_frames / 2u) / coded_frames;
    return rate >= 8000u && rate <= 192000u ? (uint32_t)rate : fallback_rate;
}

static int32_t audio_sink_open(backend_state_t *state, uint32_t input_rate, uint32_t channels)
{
    int32_t result;
    int volumes[8];
    uint32_t index;

    if (input_rate < 8000u || input_rate > 192000u || channels == 0 || channels > 2)
        return IPTV_NATIVE_E_AUDIO_FRAME;
    result = sceAudioOutInit();
    if (result < 0 && (uint32_t)result != UINT32_C(0x8026000e))
        return result;
    state->audio_sink.handle =
        sceAudioOutOpen(0xff, 0, 0, AUDIO_OUT_GRAIN, AUDIO_OUT_RATE, AUDIO_OUT_STEREO_S16);
    if (state->audio_sink.handle < 0)
        return state->audio_sink.handle;
    state->audio_sink.input_rate = input_rate;
    state->audio_sink.channels = channels;
    for (index = 0; index < 8; ++index)
        volumes[index] = AUDIO_OUT_VOLUME_0DB;
    result = sceAudioOutSetVolume(state->audio_sink.handle, 3, volumes);
    return result < 0 ? result : 0;
}

static int32_t audio_output_frame(backend_state_t *state, int16_t left, int16_t right)
{
    audio_sink_t *sink = &state->audio_sink;
    int32_t result;

    sink->block[sink->pending++] = left;
    sink->block[sink->pending++] = right;
    if (sink->pending != AUDIO_OUT_GRAIN * 2u)
        return 0;
    result = sceAudioOutOutput(sink->handle, sink->block);
    if (result < 0)
    {
        ++state->telemetry.audio_output_errors;
        return result;
    }
    ++state->telemetry.audio_output_grains;
    sink->pending = 0;
    return 0;
}

static int32_t audio_push_pcm(backend_state_t *state, const int16_t *samples, uint32_t sample_count)
{
    audio_sink_t *sink = &state->audio_sink;
    uint32_t frames = sample_count / sink->channels;
    uint32_t index;

    for (index = 0; index < frames; ++index)
    {
        int16_t left = samples[index * sink->channels];
        int16_t right = sink->channels == 2 ? samples[index * 2u + 1u] : left;
        if (!sink->have_previous)
        {
            sink->previous_left = left;
            sink->previous_right = right;
            sink->have_previous = 1;
            sink->input_index = 0;
            continue;
        }

        ++sink->input_index;
        {
            uint64_t interval_end = sink->input_index * AUDIO_OUT_RATE;
            uint64_t interval_start = (sink->input_index - 1u) * AUDIO_OUT_RATE;
            while (sink->next_output_position < interval_end)
            {
                uint64_t fraction = sink->next_output_position - interval_start;
                int32_t out_left =
                    sink->previous_left +
                    (int32_t)(((int64_t)(left - sink->previous_left) * (int64_t)fraction) /
                              AUDIO_OUT_RATE);
                int32_t out_right =
                    sink->previous_right +
                    (int32_t)(((int64_t)(right - sink->previous_right) * (int64_t)fraction) /
                              AUDIO_OUT_RATE);
                int32_t result = audio_output_frame(state, (int16_t)out_left, (int16_t)out_right);
                if (result < 0)
                    return result;
                sink->next_output_position += sink->input_rate;
            }
        }
        sink->previous_left = left;
        sink->previous_right = right;
    }
    return 0;
}

static int32_t audio_drain(backend_state_t *state)
{
    audio_sink_t *sink = &state->audio_sink;
    int32_t first_result = 0;
    int32_t result;

    if (sink->handle < 0 || sink->drained)
        return 0;
    if (sink->pending != 0)
    {
        memset(sink->block + sink->pending, 0,
               (AUDIO_OUT_GRAIN * 2u - sink->pending) * sizeof(int16_t));
        result = sceAudioOutOutput(sink->handle, sink->block);
        if (result < 0)
        {
            first_result = result;
            ++state->telemetry.audio_output_errors;
        }
        else
        {
            ++state->telemetry.audio_output_grains;
        }
        sink->pending = 0;
    }
    result = sceAudioOutOutput(sink->handle, NULL);
    if (first_result == 0 && result < 0)
        first_result = result;
    sink->drained = 1;
    return first_result;
}

static int32_t initialize_audio(backend_state_t *state)
{
    int32_t result = sceSysmoduleLoadModule(AUDIO_MODULE_ID);

    if (result < 0)
        return result;
    state->audio_module_loaded = 1;
    result = sceAudiodecInitLibrary(AUDIODEC_AAC);
    if (result < 0)
        return result;
    state->audio_library_initialized = 1;

    state->audio_param =
        (sce_audiodec_param_aac_t){sizeof(state->audio_param), AUDIODEC_WORD_S16, 1, 4, 2, 1};
    memset(&state->audio_info, 0, sizeof(state->audio_info));
    state->audio_info.size = sizeof(state->audio_info);
    state->audio_au.size = sizeof(state->audio_au);
    state->audio_pcm_item.size = sizeof(state->audio_pcm_item);
    state->audio_ctrl.param = &state->audio_param;
    state->audio_ctrl.stream_info = &state->audio_info;
    state->audio_ctrl.au_info = &state->audio_au;
    state->audio_ctrl.pcm_item = &state->audio_pcm_item;
    state->audio_decoder = sceAudiodecCreateDecoder(&state->audio_ctrl, AUDIODEC_AAC);
    return state->audio_decoder < 0 ? state->audio_decoder : 0;
}

static int32_t pace_before_present(backend_state_t *state, uint64_t pts_us)
{
    uint64_t now = monotonic_us();
    uint64_t target;
    uint64_t wait_started;
    uint64_t waited;
    int reset = 0;

    if (pts_us == UINT64_MAX)
        return 0;

    if (!state->pace_active || pts_us < state->pace_base_pts_us ||
        pts_us + PACE_BACKWARD_TOLERANCE_US < state->pace_last_pts_us ||
        (pts_us > state->pace_last_pts_us &&
         pts_us - state->pace_last_pts_us > PACE_DISCONTINUITY_US))
        reset = 1;

    if (!reset)
    {
        target = state->pace_base_clock_us + (pts_us - state->pace_base_pts_us);
        if ((target > now && target - now > PACE_MAX_WAIT_US) ||
            (now > target && now - target > PACE_DISCONTINUITY_US))
            reset = 1;
    }

    if (reset)
    {
        state->pace_active = 1;
        state->pace_base_pts_us = pts_us;
        state->pace_base_clock_us = now;
        state->pace_last_pts_us = pts_us;
        ++state->telemetry.pacing_resets;
        return 0;
    }

    target = state->pace_base_clock_us + (pts_us - state->pace_base_pts_us);
    state->pace_last_pts_us = pts_us;
    if (target <= now)
    {
        uint64_t late = now - target;
        ++state->telemetry.pacing_late_frames;
        if (late > state->telemetry.pacing_max_late_us)
            state->telemetry.pacing_max_late_us = late;
        return 0;
    }

    ++state->telemetry.pacing_waits;
    wait_started = now;
    while (now < target)
    {
        uint64_t remaining = target - now;
        uint32_t slice =
            remaining > PACE_SLEEP_SLICE_US ? PACE_SLEEP_SLICE_US : (uint32_t)remaining;
        int32_t result;

        if (atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
            return IPTV_NATIVE_E_CANCELLED;
        result = sceKernelUsleep(slice);
        if (result < 0)
            return result;
        now = monotonic_us();
    }
    waited = now - wait_started;
    state->telemetry.pacing_wait_total_us += waited;
    if (waited > state->telemetry.pacing_wait_max_us)
        state->telemetry.pacing_wait_max_us = waited;
    return 0;
}

static int32_t initialize_video(backend_state_t *state)
{
    videodec2_compute_config_t compute_config = {0};
    videodec2_decoder_config_t decoder_config = {0};
    int64_t direct_limit;
    size_t available = 0;
    uint8_t mapping_info[0x48] = {0};
    int32_t result;

    result = sceSysmoduleLoadModule(VIDEO_MODULE_ID);
    if (result != 0)
        return result;
    state->video_module_loaded = 1;

    direct_limit = sceKernelGetDirectMemorySize();
    if (direct_limit <= 0)
        return IPTV_NATIVE_E_STATE;

    state->compute_memory.size = sizeof(state->compute_memory);
    result = sceVideodec2QueryComputeMemoryInfo(&state->compute_memory);
    if (result != 0)
        return result;
    state->compute_allocation.size = align_16k((size_t)state->compute_memory.cpu_gpu_size);
    result = allocate_direct(&state->compute_allocation, state->compute_allocation.size, 0x33,
                             direct_limit);
    if (result != 0)
        return result;
    state->compute_memory.cpu_gpu = state->compute_allocation.address;
    state->compute_memory.cpu_gpu_size = state->compute_allocation.size;
    compute_config.size = sizeof(compute_config);
    result = sceVideodec2AllocateComputeQueue(&compute_config, &state->compute_memory,
                                              &state->compute_queue);
    if (result != 0)
        return result;

    decoder_config.size = sizeof(decoder_config);
    decoder_config.resource_type = 1;
    decoder_config.codec_type = state->mode->decoder_codec;
    decoder_config.profile = state->mode->decoder_profile;
    decoder_config.max_level = state->mode->max_level;
    decoder_config.max_width = (int32_t)state->mode->decoder_max_width;
    decoder_config.max_height = (int32_t)state->mode->decoder_max_height;
    decoder_config.max_dpb_frames = 4;
    decoder_config.pipeline_depth = state->config.codec == IPTV_NATIVE_CODEC_VP9_PROFILE0 ? 3u : 1u;
    decoder_config.compute_queue = (uint64_t)state->compute_queue;
    decoder_config.cpu_affinity = 0x3f;
    decoder_config.cpu_priority = 700;
    decoder_config.optimize_progressive = 1;

    state->decoder_memory.size = sizeof(state->decoder_memory);
    result = sceVideodec2QueryDecoderMemoryInfo(&decoder_config, &state->decoder_memory);
    if (result != 0)
        return result;

    state->cpu_mapping_size = align_16k((size_t)state->decoder_memory.cpu_size);
    result = sceKernelAvailableFlexibleMemorySize(&available);
    if (result == 0 && available < state->cpu_mapping_size)
        result = IPTV_NATIVE_E_STATE;
    if (result == 0)
        result = sceKernelMapNamedFlexibleMemory(&state->decoder_memory.cpu,
                                                 state->cpu_mapping_size, 0x03, 0, "IptvVdecCpu");
    if (result != 0)
        return result;
    result =
        sceKernelVirtualQuery(state->decoder_memory.cpu, 0, mapping_info, sizeof(mapping_info));
    if (result != 0)
        return result;

    state->gpu_allocation.size = align_16k((size_t)state->decoder_memory.gpu_size);
    state->cpu_gpu_allocation.size = align_16k((size_t)state->decoder_memory.cpu_gpu_size);
    state->input_slot_size = INPUT_SLOT_BYTES;
    state->frame_slot_size = align_16k((size_t)state->decoder_memory.max_frame_size);
    if (state->gpu_allocation.size == 0 || state->frame_slot_size == 0)
        return IPTV_NATIVE_E_STATE;

    result =
        allocate_direct(&state->gpu_allocation, state->gpu_allocation.size, 0x32, direct_limit);
    if (result == 0 && state->cpu_gpu_allocation.size != 0)
        result = allocate_direct(&state->cpu_gpu_allocation, state->cpu_gpu_allocation.size, 0x33,
                                 direct_limit);
    if (result == 0)
        result =
            allocate_direct(&state->input_allocation,
                            state->input_slot_size * PIPELINE_BUFFER_COUNT, 0x32, direct_limit);
    if (result == 0)
        result =
            allocate_direct(&state->frame_allocation,
                            state->frame_slot_size * PIPELINE_BUFFER_COUNT, 0x32, direct_limit);
    if (result != 0)
        return result;

    state->decoder_memory.gpu = state->gpu_allocation.address;
    state->decoder_memory.gpu_size = state->gpu_allocation.size;
    if (state->cpu_gpu_allocation.size != 0)
    {
        state->decoder_memory.cpu_gpu = state->cpu_gpu_allocation.address;
        state->decoder_memory.cpu_gpu_size = state->cpu_gpu_allocation.size;
    }
    result = sceVideodec2CreateDecoder(&decoder_config, &state->decoder_memory, &state->decoder);
    if (result == 0)
        result = sceVideodec2Reset(state->decoder);
    return result;
}

int32_t iptv_native_backend_init(iptv_native_backend_t *backend)
{
    backend_state_t *state;

    if (!backend)
        return IPTV_NATIVE_E_ARGUMENT;
    memset(backend, 0, sizeof(*backend));
    state = state_from(backend);
    state->magic = BACKEND_MAGIC;
    state->state = IPTV_NATIVE_STATE_IDLE;
    state->audio_decoder = -1;
    state->audio_sink.handle = -1;
    reset_allocation(&state->compute_allocation);
    reset_allocation(&state->gpu_allocation);
    reset_allocation(&state->cpu_gpu_allocation);
    reset_allocation(&state->input_allocation);
    reset_allocation(&state->frame_allocation);
    state->telemetry.state = state->state;
    return 0;
}

int32_t iptv_native_backend_open(iptv_native_backend_t *backend,
                                 const iptv_native_open_config_t *config)
{
    backend_state_t *state = state_from(backend);
    const native_video_mode_t *mode;
    int32_t result;

    if (!state || state->magic != BACKEND_MAGIC || !config)
        return IPTV_NATIVE_E_ARGUMENT;
    if (state->state != IPTV_NATIVE_STATE_IDLE)
        return IPTV_NATIVE_E_STATE;
    mode = find_video_mode(config);
    if (!mode || config->bit_depth != 8 || config->chroma_format != IPTV_NATIVE_CHROMA_420 ||
        config->hdr != 0 || !profile_supported(config, mode))
    {
        state->telemetry.last_result = IPTV_NATIVE_E_UNSUPPORTED;
        return IPTV_NATIVE_E_UNSUPPORTED;
    }

    state->mode = mode;
    state->config = *config;
    state->open_started_us = monotonic_us();
    state->telemetry.codec = config->codec;
    state->telemetry.profile = config->profile;
    state->telemetry.level = config->level;
    state->telemetry.coded_width = config->coded_width;
    state->telemetry.coded_height = config->coded_height;
    state->telemetry.visible_width = config->visible_width;
    state->telemetry.visible_height = config->visible_height;
    state->telemetry.output_pitch = mode->output_pitch;
    state->telemetry.output_surface_height = mode->output_height;
    iptv_native_agc_present_set_cancelled(0);

    result = initialize_video(state);
    if (result == 0 && config->enable_audio)
        result = initialize_audio(state);
    if (result != 0)
    {
        state->telemetry.last_native_result = result;
        state->telemetry.last_result = result;
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
        (void)iptv_native_backend_close(backend);
        state->telemetry.last_native_result = result;
        state->telemetry.last_result = result;
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
        return result;
    }

    state->telemetry.input_slot_bytes = state->input_slot_size;
    state->telemetry.frame_slot_bytes = state->frame_slot_size;
    state->state = IPTV_NATIVE_STATE_OPEN;
    state->telemetry.state = state->state;
    return 0;
}

static int32_t present_video_output(backend_state_t *state, const videodec2_frame_t *frame,
                                    const videodec2_output_t *output, int require_accepted,
                                    int from_drain)
{
    uint64_t presentation_pts_us;
    int displayable;
    uint64_t started;
    uint64_t elapsed;
    int32_t result;

    if (!output->valid || output->error || (require_accepted && !frame->accepted) ||
        output->picture_count != 1 || output->codec != state->mode->decoder_codec ||
        output->width != state->mode->output_width ||
        (output->height != state->mode->output_height &&
         (!state->mode->alternate_output_height ||
          output->height != state->mode->alternate_output_height)) ||
        output->pitch != state->mode->output_pitch || !output->buffer ||
        (output->pitch_bytes != 0 && output->pitch_bytes != output->pitch) ||
        output->buffer_size == 0 || output->buffer_size > state->frame_slot_size ||
        !frame_is_in_pool(state, output->buffer))
    {
        ++state->telemetry.decoder_errors;
        state->telemetry.last_result = IPTV_NATIVE_E_DECODER_OUTPUT;
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
        return IPTV_NATIVE_E_DECODER_OUTPUT;
    }
    if (!state_pending_take(state, &presentation_pts_us, &displayable))
    {
        ++state->telemetry.decoder_errors;
        state->telemetry.last_result = IPTV_NATIVE_E_DECODER_OUTPUT;
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
        return IPTV_NATIVE_E_DECODER_OUTPUT;
    }

    ++state->telemetry.decoded_frames;
    state->telemetry.last_decoder_output = (uintptr_t)output->buffer;
    state->telemetry.decoder_output_in_frame_pool = 1;
    if (!displayable)
    {
        ++state->telemetry.hidden_decoded_frames;
        if (from_drain)
            ++state->telemetry.drained_video_frames;
        return 0;
    }
    result = pace_before_present(state, presentation_pts_us);
    if (result != 0)
        goto failed;

    started = monotonic_us();
    state->telemetry.last_present_source = (uintptr_t)output->buffer;
    state->telemetry.zero_copy_pointer_match =
        state->telemetry.last_decoder_output == state->telemetry.last_present_source;
    result = iptv_native_agc_present_nv12(output->buffer, (size_t)output->buffer_size,
                                          output->pitch, output->height, state->mode->visible_width,
                                          state->mode->visible_height);
    elapsed = monotonic_us() - started;
    state->telemetry.present_total_us += elapsed;
    if (elapsed > state->telemetry.present_max_us)
        state->telemetry.present_max_us = elapsed;
    if (result != 0)
        goto failed;

    state->telemetry.last_presented_video_pts_us = presentation_pts_us;
    ++state->telemetry.presented_frames;
    if (state->telemetry.decoder_output_in_frame_pool && state->telemetry.zero_copy_pointer_match)
        state->telemetry.hardware_validated = 1;
    if (from_drain)
        ++state->telemetry.drained_video_frames;
    if (state->telemetry.presented_frames == 1)
        state->telemetry.first_frame_latency_us = monotonic_us() - state->open_started_us;
    return 0;

failed:
    state->telemetry.last_native_result = result;
    state->telemetry.last_result = result;
    if (from_drain && result == IPTV_NATIVE_E_CANCELLED)
        ++state->telemetry.dropped_delayed_frames;
    if (result != IPTV_NATIVE_E_CANCELLED)
    {
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
    }
    return result;
}

static int32_t submit_coded_frame(backend_state_t *state, const void *coded_frame,
                                  size_t frame_bytes, uint64_t pts_us, int displayable)
{
    videodec2_input_t input = {0};
    videodec2_frame_t frame = {0};
    videodec2_output_t output = {0};
    uint32_t slot;
    uint8_t *input_slot;
    void *frame_slot;
    uint64_t started;
    uint64_t elapsed;
    int32_t result;

    if (state->pending_pts.count == PENDING_PTS_CAPACITY)
    {
        ++state->telemetry.decoder_errors;
        state->telemetry.last_result = IPTV_NATIVE_E_DECODER_OUTPUT;
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
        return IPTV_NATIVE_E_DECODER_OUTPUT;
    }

    slot = (uint32_t)(state->telemetry.submitted_video_access_units % PIPELINE_BUFFER_COUNT);
    input_slot = (uint8_t *)state->input_allocation.address + slot * state->input_slot_size;
    frame_slot = (uint8_t *)state->frame_allocation.address + slot * state->frame_slot_size;
    memcpy(input_slot, coded_frame, frame_bytes);

    input.size = sizeof(input);
    input.au = input_slot;
    input.au_size = frame_bytes;
    input.pts = pts_us;
    input.dts = UINT64_MAX;
    frame.size = sizeof(frame);
    frame.buffer = frame_slot;
    frame.buffer_size = state->frame_slot_size;
    output.size = sizeof(output);

    started = monotonic_us();
    result = sceVideodec2Decode(state->decoder, &input, &frame, &output);
    elapsed = monotonic_us() - started;
    state->telemetry.decode_total_us += elapsed;
    if (elapsed > state->telemetry.decode_max_us)
        state->telemetry.decode_max_us = elapsed;
    ++state->telemetry.submitted_video_access_units;
    state->telemetry.submitted_video_bytes += frame_bytes;
    state->telemetry.last_video_pts_us = pts_us;

    if (result != 0 || output.error)
    {
        ++state->telemetry.decoder_errors;
        state->telemetry.last_native_result = result;
        state->telemetry.last_result = result != 0 ? result : IPTV_NATIVE_E_DECODER_OUTPUT;
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
        return state->telemetry.last_result;
    }
    if (!state_pending_push(state, pts_us, displayable))
    {
        ++state->telemetry.decoder_errors;
        state->telemetry.last_result = IPTV_NATIVE_E_DECODER_OUTPUT;
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
        return state->telemetry.last_result;
    }
    if (!output.valid)
    {
        ++state->telemetry.buffered_video_access_units;
        return 0;
    }
    return present_video_output(state, &frame, &output, 1, 0);
}

int32_t iptv_native_backend_submit_video(iptv_native_backend_t *backend, const void *coded_packet,
                                         size_t access_unit_bytes, uint64_t pts_us)
{
    backend_state_t *state = state_from(backend);
    const uint8_t *packet_bytes = coded_packet;
    iptv_vp9_packet_t vp9_packet;
    iptv_vp9_frame_flags_t vp9_flags[IPTV_VP9_MAX_SUPERFRAME_FRAMES];
    uint32_t frame_index;

    if (!state || state->magic != BACKEND_MAGIC)
        return IPTV_NATIVE_E_ARGUMENT;
    if (state->state != IPTV_NATIVE_STATE_OPEN || !state->decoder || state->drain_started)
        return IPTV_NATIVE_E_STATE;
    if (atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
        return IPTV_NATIVE_E_CANCELLED;
    if (!coded_packet || access_unit_bytes == 0 || access_unit_bytes > state->input_slot_size)
    {
        ++state->telemetry.rejected_video_access_units;
        return IPTV_NATIVE_E_ACCESS_UNIT;
    }

    if (state->config.codec != IPTV_NATIVE_CODEC_VP9_PROFILE0)
    {
        if (!annex_b_has_vcl(state->config.codec, packet_bytes, access_unit_bytes))
        {
            ++state->telemetry.rejected_video_access_units;
            return IPTV_NATIVE_E_ACCESS_UNIT;
        }
        return submit_coded_frame(state, coded_packet, access_unit_bytes, pts_us, 1);
    }

    if (iptv_vp9_split_packet(coded_packet, access_unit_bytes, &vp9_packet) != 0)
    {
        ++state->telemetry.rejected_video_access_units;
        return IPTV_NATIVE_E_ACCESS_UNIT;
    }
    for (frame_index = 0; frame_index < vp9_packet.count; ++frame_index)
    {
        if (vp9_packet.frames[frame_index].bytes > state->input_slot_size ||
            iptv_vp9_read_frame_flags(vp9_packet.frames[frame_index].data,
                                      vp9_packet.frames[frame_index].bytes,
                                      IPTV_NATIVE_VP9_PROFILE_0, &vp9_flags[frame_index]) != 0)
        {
            ++state->telemetry.rejected_video_access_units;
            return IPTV_NATIVE_E_ACCESS_UNIT;
        }
    }
    for (frame_index = 0; frame_index < vp9_packet.count; ++frame_index)
    {
        int32_t result = submit_coded_frame(state, vp9_packet.frames[frame_index].data,
                                            vp9_packet.frames[frame_index].bytes, pts_us,
                                            vp9_flags[frame_index].displayable);
        if (result != 0)
            return result;
    }
    return 0;
}

int32_t iptv_native_backend_submit_audio(iptv_native_backend_t *backend, const void *adts_frame,
                                         size_t frame_bytes, uint64_t pts_us)
{
    backend_state_t *state = state_from(backend);
    const uint8_t *adts = adts_frame;
    size_t declared_bytes;
    uint32_t channels;
    uint32_t pcm_rate;
    int32_t result;

    if (!state || state->magic != BACKEND_MAGIC || !adts_frame)
        return IPTV_NATIVE_E_ARGUMENT;
    if (state->state != IPTV_NATIVE_STATE_OPEN || !state->config.enable_audio ||
        state->audio_decoder < 0 || state->drain_started)
        return IPTV_NATIVE_E_STATE;
    if (atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
        return IPTV_NATIVE_E_CANCELLED;
    if (frame_bytes < 7 || frame_bytes > AUDIO_FRAME_MAX_BYTES || adts[0] != 0xffu ||
        (adts[1] & 0xf6u) != 0xf0u)
        return IPTV_NATIVE_E_AUDIO_FRAME;
    declared_bytes =
        ((size_t)(adts[3] & 3u) << 11) | ((size_t)adts[4] << 3) | ((size_t)adts[5] >> 5);
    channels = adts_channels(adts, frame_bytes);
    if (declared_bytes != frame_bytes || adts_core_rate(adts, frame_bytes) == 0 || channels == 0 ||
        channels > 2)
        return IPTV_NATIVE_E_AUDIO_FRAME;

    state->audio_au.address = (void *)adts_frame;
    state->audio_au.length = (uint32_t)frame_bytes;
    state->audio_pcm_item.address = state->audio_pcm;
    state->audio_pcm_item.length = sizeof(state->audio_pcm);
    result = sceAudiodecDecode(state->audio_decoder, &state->audio_ctrl);
    ++state->telemetry.submitted_audio_frames;
    state->telemetry.last_audio_pts_us = pts_us;
    if (result < 0)
        goto failed;
    if (state->audio_pcm_item.length == 0)
        return 0;
    if (state->audio_pcm_item.length > sizeof(state->audio_pcm) ||
        state->audio_info.channel_count == 0 || state->audio_info.channel_count > 2 ||
        state->audio_pcm_item.length % (sizeof(int16_t) * state->audio_info.channel_count) != 0)
    {
        result = IPTV_NATIVE_E_AUDIO_FRAME;
        goto failed;
    }

    pcm_rate = decoded_pcm_rate(adts, frame_bytes, state->audio_info.channel_count,
                                state->audio_pcm_item.length, state->audio_info.sampling_frequency);
    if (state->audio_sink.handle < 0)
    {
        result = audio_sink_open(state, pcm_rate, state->audio_info.channel_count);
        if (result < 0)
            goto failed;
    }
    else if (state->audio_sink.input_rate != pcm_rate ||
             state->audio_sink.channels != state->audio_info.channel_count)
    {
        result = IPTV_NATIVE_E_AUDIO_FRAME;
        goto failed;
    }

    result = audio_push_pcm(state, (const int16_t *)state->audio_pcm,
                            state->audio_pcm_item.length / sizeof(int16_t));
    if (result < 0)
        goto failed;
    ++state->telemetry.decoded_audio_frames;
    return 0;

failed:
    state->telemetry.last_native_result = result;
    state->telemetry.last_result = result;
    state->state = IPTV_NATIVE_STATE_ERROR;
    state->telemetry.state = state->state;
    return result;
}

void iptv_native_backend_request_stop(iptv_native_backend_t *backend)
{
    backend_state_t *state = state_from(backend);

    if (!state || state->magic != BACKEND_MAGIC)
        return;
    atomic_store_explicit(&state->stop_requested, 1, memory_order_relaxed);
    state->telemetry.stop_requested = 1;
    iptv_native_agc_present_set_cancelled(1);
}

int iptv_native_backend_stop_requested(const iptv_native_backend_t *backend)
{
    const backend_state_t *state = const_state_from(backend);

    if (!state || state->magic != BACKEND_MAGIC)
        return 1;
    return atomic_load_explicit(&state->stop_requested, memory_order_relaxed) != 0;
}

static int32_t drain_video(backend_state_t *state)
{
    uint32_t flush_call;

    if (state->video_drained)
        return 0;
    if (!state->decoder || state->state == IPTV_NATIVE_STATE_ERROR ||
        atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
    {
        discard_pending_video(state);
        state->video_drained = 1;
        return 0;
    }

    for (flush_call = 0; flush_call < VIDEO_DRAIN_FLUSH_LIMIT && state->pending_pts.count != 0;
         ++flush_call)
    {
        videodec2_frame_t frame = {0};
        videodec2_output_t output = {0};
        uint32_t slot = (uint32_t)(state->telemetry.decoder_flushes % PIPELINE_BUFFER_COUNT);
        uint64_t started;
        uint64_t elapsed;
        int32_t result;

        if (atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
        {
            discard_pending_video(state);
            state->video_drained = 1;
            return 0;
        }

        frame.size = sizeof(frame);
        frame.buffer = (uint8_t *)state->frame_allocation.address + slot * state->frame_slot_size;
        frame.buffer_size = state->frame_slot_size;
        output.size = sizeof(output);
        started = monotonic_us();
        result = sceVideodec2Flush(state->decoder, &frame, &output);
        elapsed = monotonic_us() - started;
        state->telemetry.decode_total_us += elapsed;
        if (elapsed > state->telemetry.decode_max_us)
            state->telemetry.decode_max_us = elapsed;
        ++state->telemetry.decoder_flushes;

        if (result != 0 || output.error)
        {
            ++state->telemetry.decoder_errors;
            state->telemetry.last_native_result = result;
            state->telemetry.last_result = result != 0 ? result : IPTV_NATIVE_E_DECODER_OUTPUT;
            state->state = IPTV_NATIVE_STATE_ERROR;
            state->telemetry.state = state->state;
            return state->telemetry.last_result;
        }
        if (atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
        {
            discard_pending_video(state);
            state->video_drained = 1;
            return 0;
        }
        if (!output.valid)
        {
            discard_pending_video(state);
            state->video_drained = 1;
            return 0;
        }

        result = present_video_output(state, &frame, &output, 0, 1);
        if (result == IPTV_NATIVE_E_CANCELLED &&
            atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
        {
            discard_pending_video(state);
            state->video_drained = 1;
            return 0;
        }
        if (result != 0)
            return result;
    }

    if (state->pending_pts.count != 0)
    {
        ++state->telemetry.drain_flush_limit_hits;
        ++state->telemetry.decoder_errors;
        state->telemetry.last_result = IPTV_NATIVE_E_DECODER_OUTPUT;
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
        return IPTV_NATIVE_E_DECODER_OUTPUT;
    }
    state->video_drained = 1;
    return 0;
}

int32_t iptv_native_backend_drain(iptv_native_backend_t *backend)
{
    backend_state_t *state = state_from(backend);
    int32_t first_result;
    int32_t result;

    if (!state || state->magic != BACKEND_MAGIC)
        return IPTV_NATIVE_E_ARGUMENT;
    if (state->state != IPTV_NATIVE_STATE_OPEN && state->state != IPTV_NATIVE_STATE_STOPPING &&
        state->state != IPTV_NATIVE_STATE_ERROR)
        return state->state == IPTV_NATIVE_STATE_CLOSED ? 0 : IPTV_NATIVE_E_STATE;
    state->drain_started = 1;
    first_result = drain_video(state);
    result = audio_drain(state);
    if (first_result == 0 && result != 0)
        first_result = result;
    result = iptv_native_agc_present_drain();
    if (first_result == 0 && result != 0)
        first_result = result;
    if (first_result == 0 && state->telemetry.hardware_validated &&
        state->telemetry.decoded_frames != 0 &&
        state->telemetry.presented_frames + state->telemetry.hidden_decoded_frames ==
            state->telemetry.decoded_frames &&
        state->pending_pts.count == 0 && state->telemetry.decoder_errors == 0 &&
        (!state->config.enable_audio || state->telemetry.decoded_audio_frames != 0))
        state->telemetry.stream_acceptance_validated = 1;
    return first_result;
}

int32_t iptv_native_backend_get_telemetry(const iptv_native_backend_t *backend,
                                          iptv_native_telemetry_t *telemetry)
{
    const backend_state_t *state = const_state_from(backend);

    if (!state || state->magic != BACKEND_MAGIC || !telemetry)
        return IPTV_NATIVE_E_ARGUMENT;
    *telemetry = state->telemetry;
    telemetry->state = state->state;
    telemetry->stop_requested =
        (uint32_t)atomic_load_explicit(&state->stop_requested, memory_order_relaxed);
    return 0;
}

int32_t iptv_native_backend_close(iptv_native_backend_t *backend)
{
    backend_state_t *state = state_from(backend);
    int32_t first_result = 0;
    int32_t result;

    if (!state || state->magic != BACKEND_MAGIC)
        return IPTV_NATIVE_E_ARGUMENT;
    if (state->state == IPTV_NATIVE_STATE_CLOSED || state->state == IPTV_NATIVE_STATE_IDLE)
        return 0;
    result = iptv_native_backend_drain(backend);
    if (result != 0)
        first_result = result;
    state->state = IPTV_NATIVE_STATE_STOPPING;
    state->telemetry.state = state->state;
    iptv_native_backend_request_stop(backend);
    result = iptv_native_agc_present_shutdown();
    if (first_result == 0 && result != 0)
        first_result = result;

    if (state->audio_sink.handle >= 0)
    {
        result = sceAudioOutClose(state->audio_sink.handle);
        if (first_result == 0 && result != 0)
            first_result = result;
        state->audio_sink.handle = -1;
    }
    if (state->audio_decoder >= 0)
    {
        result = sceAudiodecDeleteDecoder(state->audio_decoder);
        if (first_result == 0 && result != 0)
            first_result = result;
        state->audio_decoder = -1;
    }
    if (state->audio_library_initialized)
    {
        result = sceAudiodecTermLibrary(AUDIODEC_AAC);
        if (first_result == 0 && result != 0)
            first_result = result;
        state->audio_library_initialized = 0;
    }
    if (state->audio_module_loaded)
    {
        result = sceSysmoduleUnloadModule(AUDIO_MODULE_ID);
        if (first_result == 0 && result != 0)
            first_result = result;
        state->audio_module_loaded = 0;
    }

    if (state->decoder)
    {
        result = sceVideodec2DeleteDecoder(state->decoder);
        if (first_result == 0 && result != 0)
            first_result = result;
        state->decoder = NULL;
    }
    result = release_direct(&state->frame_allocation);
    if (first_result == 0 && result != 0)
        first_result = result;
    result = release_direct(&state->input_allocation);
    if (first_result == 0 && result != 0)
        first_result = result;
    result = release_direct(&state->cpu_gpu_allocation);
    if (first_result == 0 && result != 0)
        first_result = result;
    result = release_direct(&state->gpu_allocation);
    if (first_result == 0 && result != 0)
        first_result = result;
    if (state->decoder_memory.cpu)
    {
        result = sceKernelReleaseFlexibleMemory(state->decoder_memory.cpu, state->cpu_mapping_size);
        if (first_result == 0 && result != 0)
            first_result = result;
        result = sceKernelMunmap(state->decoder_memory.cpu, state->cpu_mapping_size);
        if (first_result == 0 && result != 0)
            first_result = result;
        state->decoder_memory.cpu = NULL;
    }
    if (state->compute_queue)
    {
        result = sceVideodec2ReleaseComputeQueue(state->compute_queue);
        if (first_result == 0 && result != 0)
            first_result = result;
        state->compute_queue = NULL;
    }
    result = release_direct(&state->compute_allocation);
    if (first_result == 0 && result != 0)
        first_result = result;
    if (state->video_module_loaded)
    {
        result = sceSysmoduleUnloadModule(VIDEO_MODULE_ID);
        if (first_result == 0 && result != 0)
            first_result = result;
        state->video_module_loaded = 0;
    }

    state->telemetry.cleanup_result = first_result;
    if (first_result != 0)
        state->telemetry.last_result = first_result;
    state->state = IPTV_NATIVE_STATE_CLOSED;
    state->telemetry.state = state->state;
    return first_result;
}

#ifdef IPTV_NATIVE_BACKEND_STATE_TEST
int main(void)
{
    pending_pts_t pending = {0};
    uint64_t pts_us;
    int displayable;
    uint32_t index;

    /* Decode order 0, 66 ms, 33 ms models one reordered B-frame group. */
    assert(pending_pts_push(&pending, 0, 1));
    assert(pending_pts_push(&pending, 66666, 1));
    assert(pending_pts_push(&pending, 33333, 0));
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == 0 &&
           displayable);
    assert(pending.count == 2);

    assert(pending_pts_push(&pending, UINT64_MAX, 1));
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == 33333 &&
           !displayable);
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == 66666 &&
           displayable);
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == UINT64_MAX);
    assert(!pending_pts_take_smallest(&pending, &pts_us, &displayable));

    for (index = 0; index < PENDING_PTS_CAPACITY; ++index)
        assert(pending_pts_push(&pending, index, 1));
    assert(!pending_pts_push(&pending, PENDING_PTS_CAPACITY, 1));
    for (index = 0; index < VIDEO_DRAIN_FLUSH_LIMIT; ++index)
        assert(pending_pts_take_first(&pending, &pts_us, &displayable) && pts_us == index);
    assert(pending.count == 0);
    return 0;
}
#endif
