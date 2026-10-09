/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 * Native PS5 IPTV decode/audio backend. Transport and UI independent. */

#include "iptv_native_backend.h"
#include "iptv_native_agc_present.h"
#include "iptv_fields.h"
#include "iptv_vp9_packet.h"
#include "iptv_mp2.h"
#include "iptv_audio_frame.h"
#include "iptv_audio_decode.h"
#define MINIMP3_IMPLEMENTATION
#include "../vendor/minimp3/minimp3.h"

#ifdef IPTV_NATIVE_BACKEND_STATE_TEST
#include <assert.h>
#endif
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static atomic_uint master_volume = 100;

void iptv_native_set_volume(unsigned percent)
{
    atomic_store_explicit(&master_volume, percent > 100 ? 100 : percent, memory_order_relaxed);
}

unsigned iptv_native_get_volume(void)
{
    return atomic_load_explicit(&master_volume, memory_order_relaxed);
}

#define BACKEND_MAGIC UINT32_C(0x49505456)
#define PIPELINE_BUFFER_COUNT 3u
#define PENDING_PTS_CAPACITY 32u
#define VIDEO_DRAIN_FLUSH_LIMIT PENDING_PTS_CAPACITY
#define INPUT_SLOT_BYTES 0x800000u
#define AUDIODEC_AAC 3u
#define AUDIODEC_WORD_S16 1
#define AUDIO_OUT_GRAIN 256u
#define AUDIO_OUT_RATE 48000u
#define AUDIO_OUT_STEREO_S16 1u
#define AUDIO_OUT_VOLUME_0DB 0x8000
#define AUDIO_PCM_BYTES 0x4000u
#define AUDIO_FRAME_MAX_BYTES 8194u
#define AUDIO_QUEUE_CAPACITY 1024u
#define VIDEO_QUEUE_CAPACITY 512u
#define VIDEO_QUEUE_MAX_BYTES (32u * 1024u * 1024u)
#define PLAYBACK_BUFFER_US UINT64_C(2000000)
#define DEMUX_BUFFER_US (PLAYBACK_BUFFER_US + UINT64_C(1000000))
#define PLAYBACK_START_TIMEOUT_US UINT64_C(8000000)
#define PLAYBACK_UNDERRUN_GRACE_US UINT64_C(250000)
#define VIDEO_MODULE_ID 207u
#define AUDIO_MODULE_ID 0x0088u
#define PACE_SLEEP_SLICE_US 5000u
#define PACE_MAX_WAIT_US UINT64_C(500000)
#define PACE_DISCONTINUITY_US UINT64_C(2000000)
#define PACE_BACKWARD_TOLERANCE_US UINT64_C(100000)
#define PACE_REBASE_LATE_US UINT64_C(50000)
#define PACE_DROP_LATE_US UINT64_C(500000)
#define CONTROLS_OVERLAY_US UINT64_C(5000000)

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
    uint8_t optimize_progressive, check_memory_type, reserved0, reserved1;
    void *extra_config;
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

_Static_assert(sizeof(videodec2_decoder_config_t) == 72, "unexpected Videodec2 config ABI");
_Static_assert(offsetof(videodec2_decoder_config_t, optimize_progressive) == 60,
               "unexpected Videodec2 flag offset");
_Static_assert(offsetof(videodec2_decoder_config_t, extra_config) == 64,
               "unexpected Videodec2 extra-config offset");
_Static_assert(sizeof(videodec2_decoder_memory_t) == 72, "unexpected Videodec2 memory ABI");
_Static_assert(sizeof(videodec2_compute_config_t) == 16, "unexpected Videodec2 compute config ABI");
_Static_assert(sizeof(videodec2_compute_memory_t) == 24, "unexpected Videodec2 compute memory ABI");
_Static_assert(sizeof(videodec2_input_t) == 48, "unexpected Videodec2 input ABI");
_Static_assert(sizeof(videodec2_frame_t) == 32, "unexpected Videodec2 frame ABI");
_Static_assert(sizeof(videodec2_output_t) == 56, "unexpected Videodec2 output ABI");

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
    uint32_t decoder_max_width;
    uint32_t decoder_max_height;
} native_video_mode_t;

static const native_video_mode_t video_modes[] = {
    /* H.264 uses these rows for level and geometry limits. */
    {IPTV_NATIVE_CODEC_H264, 0, 1, IPTV_NATIVE_H264_PROFILE_HIGH, 41, 1280, 720},
    {IPTV_NATIVE_CODEC_H264, 0, 1, IPTV_NATIVE_H264_PROFILE_HIGH, 51, 1920, 1088},
    {IPTV_NATIVE_CODEC_H264, 0, 1, IPTV_NATIVE_H264_PROFILE_HIGH, 51, 2560, 1440},
    {IPTV_NATIVE_CODEC_H264, 0, 1, IPTV_NATIVE_H264_PROFILE_HIGH, 52, 3840, 2176},
    {IPTV_NATIVE_CODEC_HEVC, 0, 0x000ee049, IPTV_NATIVE_HEVC_PROFILE_MAIN, 123, 1280, 720},
    {IPTV_NATIVE_CODEC_HEVC, 0, 0x000ee049, IPTV_NATIVE_HEVC_PROFILE_MAIN, 123, 1920, 1088},
    {IPTV_NATIVE_CODEC_HEVC, 0, 0x000ee049, IPTV_NATIVE_HEVC_PROFILE_MAIN, 150, 2560, 1440},
    {IPTV_NATIVE_CODEC_HEVC, 0, 0x000ee049, IPTV_NATIVE_HEVC_PROFILE_MAIN, 153, 3840, 2176},
    /* Broadcast 4K (Chinese satellite channels: Main10, 50 frames a second)
     * is often signalled as level 6 or above though the picture is within
     * what 5.1 allows. initialize_video() asks the decoder for this level and
     * falls back to 5.1 when it refuses the number. */
    {IPTV_NATIVE_CODEC_HEVC, 0, 0x000ee049, IPTV_NATIVE_HEVC_PROFILE_MAIN, 186, 3840, 2176},
    {IPTV_NATIVE_CODEC_VP9_PROFILE0, IPTV_NATIVE_VP9_PROFILE_0, 0x00245bfd,
     IPTV_NATIVE_VP9_PROFILE_0, 41, 1920, 1080},
    {IPTV_NATIVE_CODEC_VP9_PROFILE0, IPTV_NATIVE_VP9_PROFILE_0, 0x00245bfd,
     IPTV_NATIVE_VP9_PROFILE_0, 50, 2560, 1440},
    {IPTV_NATIVE_CODEC_VP9_PROFILE0, IPTV_NATIVE_VP9_PROFILE_0, 0x00245bfd,
     IPTV_NATIVE_VP9_PROFILE_0, 51, 3840, 2160},
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
        iptv_field_info_t fields;
    } values[PENDING_PTS_CAPACITY];
    uint32_t count;
    iptv_field_info_t taken_fields;
} pending_pts_t;

_Static_assert(VIDEO_DRAIN_FLUSH_LIMIT >= PENDING_PTS_CAPACITY,
               "bounded drain must cover every retained timestamp");

typedef struct audio_sink
{
    int32_t handle;
    int applied_volume;
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

typedef struct audio_queue_item
{
    uint64_t pts_us;
    uint32_t bytes;
    uint32_t generation;
    uint8_t data[AUDIO_FRAME_MAX_BYTES];
} audio_queue_item_t;

typedef struct video_queue_item
{
    _Atomic uint64_t pts_us;
    uint32_t bytes;
    uint32_t generation;
    uint8_t displayable;
    uint8_t *data;
} video_queue_item_t;

typedef struct backend_state
{
    uint32_t magic;
    iptv_native_state_t state;
    _Atomic int stop_requested;
    _Atomic int paused;
    _Atomic int discard_input;
    _Atomic uint32_t stream_generation;
    _Atomic uint64_t presented_frame_count;
    _Atomic uint64_t presented_pts_us;
    _Atomic uint32_t presented_generation;
    iptv_native_picture_t pause_picture;
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
    mp3dec_t mp2_decoder;
    iptv_audio_decoder_t *software_audio;
    uint32_t audio_module_loaded;
    uint32_t audio_library_initialized;
    sce_audiodec_param_aac_t audio_param;
    sce_audiodec_aac_info_t audio_info;
    sce_audiodec_au_info_t audio_au;
    sce_audiodec_pcm_item_t audio_pcm_item;
    sce_audiodec_ctrl_t audio_ctrl;
    audio_sink_t audio_sink;
    uint8_t audio_pcm[AUDIO_PCM_BYTES];
    uint8_t audio_staged_pcm[AUDIO_PCM_BYTES];
    uint64_t audio_staged_pts_us;
    uint32_t audio_staged_bytes;
    uint32_t audio_staged_rate;
    uint32_t audio_staged_channels;
    audio_queue_item_t *audio_queue;
    void *audio_thread;
    _Atomic uint32_t audio_queue_read;
    _Atomic uint32_t audio_queue_write;
    _Atomic uint32_t audio_queue_sample_rate;
    _Atomic uint32_t audio_buffer_type;
    _Atomic int audio_worker_stop;
    _Atomic int audio_worker_discard;
    _Atomic int audio_worker_result;
    _Atomic int audio_sync_pending;
    video_queue_item_t *video_queue;
    void *video_thread;
    _Atomic uint32_t video_queue_read;
    _Atomic uint32_t video_queue_write;
    _Atomic uint64_t video_queue_bytes;
    _Atomic int video_worker_stop;
    _Atomic int video_worker_done;
    _Atomic int video_worker_result;
    _Atomic int playback_started;
    _Atomic int programme_draining;
    _Atomic uint64_t playback_gate_started_us;

    uint64_t open_started_us;
    uint64_t last_present_monotonic_us;
    uint64_t pending_present_pts_us;
    const void *pending_present_source;
    uint8_t presentation_pending;
    uint8_t pending_present_from_drain;
#if IPTV_PROBE
    uint64_t probe_next_sample_us;
    uint64_t probe_gaps_over_40ms;
    uint64_t probe_max_gap_us;
    uint64_t probe_decode_max_us;
    uint64_t probe_present_max_us;
#endif
    uint64_t pace_base_pts_us;
    uint64_t pace_base_clock_us;
    uint64_t pace_last_pts_us;
    uint64_t frame_rate_window_start_us;
    uint32_t frame_rate_x100;
    uint32_t frame_rate_window_frames;
    uint64_t controls_started_us;
    uint64_t bitrate_window_start_us;
    uint64_t bitrate_window_bytes;
    uint32_t bitrate_kbps;
    pending_pts_t pending_pts;
    iptv_field_parser_t *field_parser;
    uint64_t extra_field_presentations;
    uint64_t previous_picture_pts;
    uint32_t pause_field;
    uint8_t drain_started;
    uint8_t video_drained;
    uint8_t pace_active;
    uint32_t video_generation;
    _Atomic uint32_t audio_generation;
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
int scePthreadCreate(void **thread, const void *attributes, void *(*entry)(void *), void *argument,
                     const char *name);
int scePthreadJoin(void *thread, void **result);

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

static int32_t start_audio_worker(backend_state_t *state);
static int32_t stop_audio_worker(backend_state_t *state);
static int32_t start_video_worker(backend_state_t *state);
static int32_t stop_video_worker(backend_state_t *state);

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

static void record_media_bytes(backend_state_t *state, size_t bytes)
{
    const uint64_t now = monotonic_us();
    uint64_t elapsed;

    if (!state || !bytes || !now)
        return;
    if (!state->bitrate_window_start_us)
        state->bitrate_window_start_us = now;
    state->bitrate_window_bytes += bytes;
    elapsed = now - state->bitrate_window_start_us;
    if (elapsed < UINT64_C(1000000))
        return;
    state->bitrate_kbps = (uint32_t)(state->bitrate_window_bytes * UINT64_C(8000) / elapsed);
    state->telemetry.bitrate_kbps = state->bitrate_kbps;
    state->bitrate_window_start_us = now;
    state->bitrate_window_bytes = 0;
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

    if (!config->coded_width || !config->coded_height || !config->visible_width ||
        !config->visible_height || config->visible_width > config->coded_width ||
        config->visible_height > config->coded_height || !config->level)
        return NULL;
    for (index = 0; index < sizeof(video_modes) / sizeof(video_modes[0]); ++index)
    {
        const native_video_mode_t *mode = &video_modes[index];
        if (mode->codec == config->codec &&
            (mode->accepted_profile == 0 || mode->accepted_profile == config->profile) &&
            config->level <= mode->max_level && config->coded_width <= mode->decoder_max_width &&
            config->coded_height <= mode->decoder_max_height)
            return mode;
    }
    return NULL;
}

static int profile_supported(const iptv_native_open_config_t *config)
{
    if (config->codec == IPTV_NATIVE_CODEC_H264)
        return config->profile == 66 || config->profile == 77 || config->profile == 100;
    if (config->codec == IPTV_NATIVE_CODEC_HEVC)
        return config->profile == IPTV_NATIVE_HEVC_PROFILE_MAIN ||
               config->profile == IPTV_NATIVE_HEVC_PROFILE_MAIN10;
    return config->codec == IPTV_NATIVE_CODEC_VP9_PROFILE0 &&
           config->profile == IPTV_NATIVE_VP9_PROFILE_0;
}

static int pending_pts_push(pending_pts_t *pending, uint64_t pts_us, int displayable)
{
    if (pending->count == PENDING_PTS_CAPACITY)
        return 0;
    pending->values[pending->count].pts_us = pts_us;
    pending->values[pending->count].displayable = displayable != 0;
    pending->values[pending->count].fields = (iptv_field_info_t){0};
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
    pending->taken_fields = pending->values[smallest].fields;
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
    pending->taken_fields = pending->values[0].fields;
    --pending->count;
    for (index = 0; index < pending->count; ++index)
        pending->values[index] = pending->values[index + 1u];
    return 1;
}

/* An interlaced picture is submitted as two access units, one per field, and
 * comes out as one frame. After the frame took its timestamp (the smallest),
 * the other field's entry goes too: one without a timestamp when there is one
 * (a second field often carries none), otherwise the next smallest. */
static int pending_pts_drop_second_field(pending_pts_t *pending)
{
    uint32_t index;
    uint64_t pts_us;
    int displayable;

    if (!pending || pending->count == 0)
        return 0;
    for (index = 0; index < pending->count; ++index)
    {
        if (pending->values[index].pts_us == UINT64_MAX)
        {
            --pending->count;
            pending->values[index] = pending->values[pending->count];
            return 1;
        }
    }
    return pending_pts_take_smallest(pending, &pts_us, &displayable);
}

static int state_pending_push(backend_state_t *state, uint64_t pts_us, int displayable,
                              iptv_field_info_t fields)
{
    if (!pending_pts_push(&state->pending_pts, pts_us, displayable))
        return 0;
    state->pending_pts.values[state->pending_pts.count - 1].fields = fields;
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

/* Development: treat every 8-bit picture as interlaced, to time the blend
 * below on a console with an ordinary channel. */
static int g_force_field_blend;

void iptv_native_backend_force_field_blend(int enabled)
{
    g_force_field_blend = enabled != 0;
}

/* An interlaced picture holds two moments, 1/50 s apart, on alternate lines:
 * anything that moves shows as a comb. Each line becomes the mean of itself
 * and the one under it, which merges the two fields; the price is a slightly
 * softer picture. Done in place, on the copy the decoder handed out. */
static void blend_rows(uint8_t *plane, uint32_t pitch, uint32_t rows, uint32_t bytes_per_row)
{
    uint32_t y;
    uint32_t x;

    for (y = 0; y + 1u < rows; ++y)
    {
        uint8_t *row = plane + (size_t)y * pitch;
        const uint8_t *below = row + pitch;
        for (x = 0; x < bytes_per_row; ++x)
            row[x] = (uint8_t)((row[x] + below[x] + 1u) >> 1);
    }
}

static void blend_fields(const videodec2_output_t *output)
{
    uint8_t *luma = (uint8_t *)output->buffer;

    blend_rows(luma, output->pitch, output->height, output->width);
    /* The colour plane: half the lines, pairs of bytes, the same width. */
    blend_rows(luma + (size_t)output->pitch * output->height, output->pitch,
               (output->height + 1u) / 2u, output->width);
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

static uint32_t pcm_rate_from_pts(uint32_t pcm_bytes, uint32_t channels, uint64_t first_pts_us,
                                  uint64_t next_pts_us, uint32_t fallback_rate)
{
    static const uint32_t rates[] = {8000u,  11025u, 12000u, 16000u, 22050u, 24000u,  32000u,
                                     44100u, 48000u, 64000u, 88200u, 96000u, 176400u, 192000u};
    uint64_t frames;
    uint64_t delta;
    uint64_t measured;
    uint32_t nearest = 0;
    uint64_t nearest_difference = UINT64_MAX;
    size_t index;

    if (channels == 0 || first_pts_us == UINT64_MAX || next_pts_us == UINT64_MAX ||
        next_pts_us <= first_pts_us)
        return fallback_rate;
    frames = pcm_bytes / (sizeof(int16_t) * channels);
    delta = next_pts_us - first_pts_us;
    if (frames == 0 || delta > UINT64_C(600000))
        return fallback_rate;
    measured = (frames * UINT64_C(1000000) + delta / 2u) / delta;
    for (index = 0; index < sizeof(rates) / sizeof(rates[0]); ++index)
    {
        const uint64_t difference =
            measured > rates[index] ? measured - rates[index] : rates[index] - measured;
        if (difference < nearest_difference)
        {
            nearest = rates[index];
            nearest_difference = difference;
        }
    }
    return nearest && nearest_difference * 100u <= (uint64_t)nearest * 3u ? nearest : fallback_rate;
}

static int32_t audio_sink_volume(audio_sink_t *sink)
{
    const unsigned percent = iptv_native_get_volume();
    if (sink->applied_volume == (int)percent)
        return 0;
    int volumes[8];
    for (unsigned index = 0; index < 8; ++index)
        volumes[index] = (int)(AUDIO_OUT_VOLUME_0DB * percent / 100u);
    const int result = sceAudioOutSetVolume(sink->handle, 3, volumes);
    if (result >= 0)
        sink->applied_volume = (int)percent;
    return result;
}

static int32_t audio_sink_open(backend_state_t *state, uint32_t input_rate, uint32_t channels)
{
    int32_t result;

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
    state->audio_sink.applied_volume = -1;
    result = audio_sink_volume(&state->audio_sink);
    return result < 0 ? result : 0;
}

static int32_t audio_output_frame(backend_state_t *state, int16_t left, int16_t right)
{
    audio_sink_t *sink = &state->audio_sink;
    uint64_t started;
    uint64_t elapsed;
    int32_t result;

    sink->block[sink->pending++] = left;
    sink->block[sink->pending++] = right;
    if (sink->pending != AUDIO_OUT_GRAIN * 2u)
        return 0;
    started = monotonic_us();
    result = audio_sink_volume(sink);
    if (result < 0)
        return result;
    result = sceAudioOutOutput(sink->handle, sink->block);
    elapsed = monotonic_us() - started;
    state->telemetry.audio_output_total_us += elapsed;
    if (elapsed > state->telemetry.audio_output_max_us)
        state->telemetry.audio_output_max_us = elapsed;
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

    result = stop_audio_worker(state);
    if (result != 0)
        first_result = result;
    if (sink->handle < 0 && state->audio_staged_bytes != 0)
    {
        const uint32_t sample_count = state->audio_staged_bytes / sizeof(int16_t);
        result = audio_sink_open(state, state->audio_staged_rate, state->audio_staged_channels);
        if (result < 0)
        {
            state->audio_staged_bytes = 0;
            return result;
        }
        state->audio_staged_bytes = 0;
        result = audio_push_pcm(state, (const int16_t *)state->audio_staged_pcm, sample_count);
        if (result < 0)
            return result;
    }
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

static int32_t create_aac_decoder(backend_state_t *state)
{
    state->audio_param =
        (sce_audiodec_param_aac_t){sizeof(state->audio_param), AUDIODEC_WORD_S16, 1, 4, 2, 1};
    memset(&state->audio_info, 0, sizeof(state->audio_info));
    memset(&state->audio_au, 0, sizeof(state->audio_au));
    memset(&state->audio_pcm_item, 0, sizeof(state->audio_pcm_item));
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

static int32_t reset_native_audio_decoder(backend_state_t *state)
{
    if (state->audio_decoder < 0 || state->software_audio)
        return 0;
    // Recreate the AAC context so overlap and prediction state cannot cross a seek.
    const int32_t result = sceAudiodecDeleteDecoder(state->audio_decoder);
    if (result < 0)
        return result;
    state->audio_decoder = -1;
    return create_aac_decoder(state);
}

static int32_t initialize_audio(backend_state_t *state)
{
    if (iptv_audio_software_type(state->config.audio_stream_type))
        return 0;
    if (state->config.audio_stream_type == 0x03u || state->config.audio_stream_type == 0x04u)
    {
        mp3dec_init(&state->mp2_decoder);
        return 0;
    }
    int32_t result = sceSysmoduleLoadModule(AUDIO_MODULE_ID);

    if (result < 0)
        return result;
    state->audio_module_loaded = 1;
    result = sceAudiodecInitLibrary(AUDIODEC_AAC);
    if (result < 0)
        return result;
    state->audio_library_initialized = 1;

    return create_aac_decoder(state);
}

static int32_t release_audio(backend_state_t *state)
{
    int32_t first_result = 0;
    int32_t result;

    result = stop_audio_worker(state);
    iptv_audio_decoder_free(&state->software_audio);
    if (result != 0)
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
    state->audio_sink.pending = 0;
    state->audio_staged_bytes = 0;
    return first_result;
}

static int32_t disable_audio_internal(backend_state_t *state, int32_t result)
{
    const int32_t cleanup_result = release_audio(state);

    state->telemetry.last_audio_result = result;
    state->telemetry.audio_disabled = 1;
    if (state->telemetry.cleanup_result == 0 && cleanup_result != 0)
        state->telemetry.cleanup_result = cleanup_result;
    state->config.enable_audio = 0;
    atomic_store(&state->audio_buffer_type, 0);
    return cleanup_result;
}

static int32_t pace_before_present(backend_state_t *state, uint64_t pts_us, int *drop_frame)
{
    uint64_t now = monotonic_us();
    uint64_t target;
    uint64_t wait_started;
    uint64_t waited;
    int reset = 0;

    *drop_frame = 0;
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
        if (late >= PACE_REBASE_LATE_US)
        {
            /* A short network or segment-fetch pause permanently offsets a
             * wall-clock pace unless its base follows the recovered stream.
             * Rebase immediately; only discard the first frame after a large
             * gap, then present subsequent frames at their normal cadence. */
            state->pace_base_pts_us = pts_us;
            state->pace_base_clock_us = now;
            state->pace_last_pts_us = pts_us;
            ++state->telemetry.pacing_resets;
            if (late >= PACE_DROP_LATE_US)
            {
                *drop_frame = 1;
                ++state->telemetry.dropped_late_video_frames;
            }
        }
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

        if (atomic_load_explicit(&state->stop_requested, memory_order_relaxed) ||
            (state->config.picture_cancelled &&
             state->config.picture_cancelled(state->config.picture_context)))
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
    decoder_config.profile = state->config.codec != IPTV_NATIVE_CODEC_VP9_PROFILE0
                                 ? state->config.profile
                                 : state->mode->decoder_profile;
    decoder_config.max_level = state->mode->max_level;
    decoder_config.max_width = (int32_t)state->mode->decoder_max_width;
    decoder_config.max_height = (int32_t)state->mode->decoder_max_height;
    /* Broadcast H.264 is frequently interlaced and can retain more than the
     * four pictures used by our progressive test clips. Give 1080p AVC the
     * full spec DPB so VideoDec2 does not reject valid field-coded streams as
     * SCE_VIDEODEC2_ERROR_OVERSIZE_DECODE. Keep the proven bounded settings
     * for larger AVC and the other codecs. */
    decoder_config.max_dpb_frames =
        state->config.codec == IPTV_NATIVE_CODEC_H264 && state->mode->decoder_max_width <= 1920u
            ? 16
        : state->config.codec == IPTV_NATIVE_CODEC_H264 && state->mode->decoder_max_width <= 2560u
            ? 8
        : state->config.codec == IPTV_NATIVE_CODEC_H264 ? 6
                                                        : 4;
    /* HEVC at any resolution can need more than four DPB pictures: even
     * ordinary 1080p x265 signals five. Keep the proven six-picture budget
     * for preview and foreground decoders, not only the 4K mode. */
    if (state->config.codec == IPTV_NATIVE_CODEC_HEVC)
        decoder_config.max_dpb_frames = 6;
    decoder_config.pipeline_depth = 1u;
    decoder_config.compute_queue = (uint64_t)state->compute_queue;
    decoder_config.cpu_affinity = 0x3f;
    decoder_config.cpu_priority = 700;
    decoder_config.optimize_progressive = state->config.codec == IPTV_NATIVE_CODEC_H264 ? 0 : 1;

    state->decoder_memory.size = sizeof(state->decoder_memory);
    result = sceVideodec2QueryDecoderMemoryInfo(&decoder_config, &state->decoder_memory);
    if (result != 0 && state->config.codec == IPTV_NATIVE_CODEC_HEVC &&
        decoder_config.max_level > 153)
    {
        /* The decoder does not take a level above 5.1 as its limit: open it
         * at 5.1 and let it judge the pictures themselves. */
        decoder_config.max_level = 153;
        memset(&state->decoder_memory, 0, sizeof(state->decoder_memory));
        state->decoder_memory.size = sizeof(state->decoder_memory);
        result = sceVideodec2QueryDecoderMemoryInfo(&decoder_config, &state->decoder_memory);
    }
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
    state->previous_picture_pts = UINT64_MAX;
    atomic_store_explicit(&state->stop_requested, 0, memory_order_relaxed);
    atomic_store_explicit(&state->stream_generation, 1u, memory_order_relaxed);
    state->video_generation = 1u;
    state->audio_generation = 1u;
    atomic_store_explicit(&state->presented_frame_count, 0, memory_order_relaxed);
    atomic_store_explicit(&state->presented_pts_us, UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&state->playback_started, 0, memory_order_relaxed);
    atomic_store_explicit(&state->playback_gate_started_us, 0, memory_order_relaxed);
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
    const int main10 = config->codec == IPTV_NATIVE_CODEC_HEVC &&
                       config->profile == IPTV_NATIVE_HEVC_PROFILE_MAIN10;
    if (!mode || (config->bit_depth != 8 && !(main10 && config->bit_depth == 10)) ||
        config->chroma_format != IPTV_NATIVE_CHROMA_420 || config->hdr != 0 ||
        !profile_supported(config))
    {
        state->telemetry.last_result = IPTV_NATIVE_E_UNSUPPORTED;
        return IPTV_NATIVE_E_UNSUPPORTED;
    }

    state->mode = mode;
    state->config = *config;
    atomic_store(&state->audio_buffer_type,
                 state->config.enable_audio
                     ? (config->audio_stream_type ? config->audio_stream_type : 0x0fu)
                     : 0);
    state->open_started_us = monotonic_us();
    state->telemetry.codec = config->codec;
    state->telemetry.profile = config->profile;
    state->telemetry.level = config->level;
    state->telemetry.coded_width = config->coded_width;
    state->telemetry.coded_height = config->coded_height;
    state->telemetry.visible_width = config->visible_width;
    state->telemetry.visible_height = config->visible_height;
    state->telemetry.output_pitch = 0;
    state->telemetry.output_surface_height = 0;
    if (!config->picture)
        iptv_native_agc_present_set_cancelled(0);

    result = initialize_video(state);
    if (result == 0 &&
        (config->codec == IPTV_NATIVE_CODEC_H264 || config->codec == IPTV_NATIVE_CODEC_HEVC))
    {
        state->field_parser = iptv_field_parser_create(config->codec);
        if (!state->field_parser)
            result = -12;
    }
    if (result == 0)
        result = start_video_worker(state);
    if (result == 0 && state->config.enable_audio)
    {
        int32_t audio_result = initialize_audio(state);
        if (audio_result == 0)
            audio_result = start_audio_worker(state);
        if (audio_result != 0)
            disable_audio_internal(state, audio_result);
    }
    if (result == 0)
        atomic_store_explicit(&state->playback_gate_started_us, monotonic_us(),
                              memory_order_release);
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

static int32_t complete_pending_presentation(backend_state_t *state)
{
    uint64_t started;
    uint64_t elapsed;
    uint64_t rate_now;
    int32_t result;

    if (!state->presentation_pending)
        return 0;
    started = monotonic_us();
    result = state->config.picture ? 0 : iptv_native_agc_present_finish_frame();
    elapsed = monotonic_us() - started;
    state->telemetry.present_total_us += elapsed;
    if (elapsed > state->telemetry.present_max_us)
        state->telemetry.present_max_us = elapsed;
#if IPTV_PROBE
    if (elapsed > state->probe_present_max_us)
        state->probe_present_max_us = elapsed;
#endif
    if (result != 0)
    {
        state->telemetry.last_native_result = result;
        state->telemetry.last_result = result;
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
        return result;
    }

    state->presentation_pending = 0;
    state->pending_present_source = NULL;
    state->telemetry.last_presented_video_pts_us = state->pending_present_pts_us;
    atomic_store_explicit(&state->presented_pts_us, state->pending_present_pts_us,
                          memory_order_release);
    atomic_store_explicit(&state->presented_generation, state->video_generation,
                          memory_order_release);
    rate_now = monotonic_us();
    if (state->last_present_monotonic_us != 0)
    {
        elapsed = rate_now - state->last_present_monotonic_us;
        if (elapsed > state->telemetry.present_gap_max_us)
            state->telemetry.present_gap_max_us = elapsed;
        if (elapsed > UINT64_C(250000))
            ++state->telemetry.present_gaps_over_250ms;
        if (elapsed > UINT64_C(500000))
            ++state->telemetry.present_gaps_over_500ms;
#if IPTV_PROBE
        if (elapsed > state->probe_max_gap_us)
            state->probe_max_gap_us = elapsed;
        if (elapsed > UINT64_C(40000))
            ++state->probe_gaps_over_40ms;
#endif
    }
    state->last_present_monotonic_us = rate_now;
    if (state->frame_rate_window_frames == 0)
        state->frame_rate_window_start_us = rate_now;
    ++state->frame_rate_window_frames;
    elapsed = rate_now - state->frame_rate_window_start_us;
    if (state->frame_rate_window_frames > 1u && elapsed >= UINT64_C(500000))
    {
        state->frame_rate_x100 = (uint32_t)(((uint64_t)state->frame_rate_window_frames - 1u) *
                                            UINT64_C(100000000) / elapsed);
        state->telemetry.actual_frame_rate_x100 = state->frame_rate_x100;
        state->frame_rate_window_start_us = rate_now;
        state->frame_rate_window_frames = 1u;
    }
    ++state->telemetry.presented_frames;
#if IPTV_PROBE
    if (state->probe_next_sample_us == 0)
        state->probe_next_sample_us = rate_now + UINT64_C(5000000);
    if (rate_now >= state->probe_next_sample_us &&
        state->telemetry.probe_sample_count < IPTV_NATIVE_PROBE_SAMPLES)
    {
        iptv_native_probe_sample_t *sample =
            &state->telemetry.probe_samples[state->telemetry.probe_sample_count++];
        const uint32_t read = atomic_load_explicit(&state->video_queue_read, memory_order_acquire);
        const uint32_t write =
            atomic_load_explicit(&state->video_queue_write, memory_order_acquire);
        sample->elapsed_ms = (uint32_t)((rate_now - state->open_started_us) / 1000u);
        sample->video_queue_frames = write - read;
        sample->presented_frames = state->telemetry.presented_frames;
        sample->pacing_late_frames = state->telemetry.pacing_late_frames;
        sample->gaps_over_40ms = state->probe_gaps_over_40ms;
        sample->max_gap_us = state->probe_max_gap_us;
        sample->decode_max_us = state->probe_decode_max_us;
        sample->present_max_us = state->probe_present_max_us;
        state->probe_next_sample_us = rate_now + UINT64_C(5000000);
        state->probe_gaps_over_40ms = 0;
        state->probe_max_gap_us = 0;
        state->probe_decode_max_us = 0;
        state->probe_present_max_us = 0;
    }
#endif
    atomic_store_explicit(&state->presented_frame_count, state->telemetry.presented_frames,
                          memory_order_release);
    if (state->telemetry.decoder_output_in_frame_pool && state->telemetry.zero_copy_pointer_match)
        state->telemetry.hardware_validated = 1;
    if (state->pending_present_from_drain)
        ++state->telemetry.drained_video_frames;
    if (state->telemetry.presented_frames == 1)
        state->telemetry.first_frame_latency_us = rate_now - state->open_started_us;
    return 0;
}

static int32_t present_video_output(backend_state_t *state, const videodec2_frame_t *frame,
                                    const videodec2_output_t *output, int require_accepted,
                                    int from_drain)
{
    uint64_t required_bytes;
    uint64_t presentation_pts_us;
    int displayable;
    uint64_t started;
    uint64_t elapsed;
    uint64_t rate_now;
    int drop_frame;
    int32_t result;
    uint32_t reject_flags = 0;

    const uint32_t component_bytes = state->config.bit_depth == 10 ? 2u : 1u;
    required_bytes = (uint64_t)output->pitch *
                     (output->height + ((uint64_t)output->height + 1u) / 2u) * component_bytes;
    state->telemetry.decoder_output_valid = output->valid;
    state->telemetry.decoder_output_error = output->error;
    state->telemetry.decoder_output_picture_count = output->picture_count;
    state->telemetry.decoder_output_codec = output->codec;
    state->telemetry.decoder_output_width = output->width;
    state->telemetry.decoder_output_height = output->height;
    state->telemetry.decoder_output_pitch = output->pitch;
    state->telemetry.decoder_frame_accepted = frame->accepted;
    if (!output->valid)
        reject_flags |= 1u << 0;
    if (output->error)
        reject_flags |= 1u << 1;
    if (require_accepted && !frame->accepted)
        reject_flags |= 1u << 2;
    /* An interlaced H.264 picture (broadcast 1080i) comes out as its two
     * fields woven into one frame of the full height: picture_count is 2 and
     * the buffer is laid out like a progressive frame's. */
    if (output->picture_count != 1 &&
        !(state->config.codec == IPTV_NATIVE_CODEC_H264 && output->picture_count == 2))
        reject_flags |= 1u << 3;
    if (output->codec != state->mode->decoder_codec)
        reject_flags |= 1u << 4;
    if (!output->width || !output->height || output->width > state->mode->decoder_max_width ||
        output->height > state->mode->decoder_max_height)
        reject_flags |= 1u << 5;
    if (output->width < state->config.visible_width ||
        output->height < state->config.visible_height)
        reject_flags |= 1u << 6;
    if (output->pitch < output->width ||
        output->pitch > ((state->mode->decoder_max_width + 255u) & ~255u) ||
        (output->pitch & 1u) != 0 || output->pitch_bytes != output->pitch * component_bytes)
        reject_flags |= 1u << 7;
    if (!output->buffer || required_bytes == 0 || output->buffer_size < required_bytes ||
        output->buffer_size > state->frame_slot_size)
        reject_flags |= 1u << 8;
    if (!frame_is_in_pool(state, output->buffer))
        reject_flags |= 1u << 9;
    if (state->config.codec == IPTV_NATIVE_CODEC_H264 &&
        output->height != state->config.coded_height &&
        output->height != state->config.visible_height)
        reject_flags |= 1u << 10;
    state->telemetry.decoder_output_reject_flags = reject_flags;
    if (reject_flags != 0)
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

    iptv_field_info_t fields = state->pending_pts.taken_fields;
    if (fields.first && !fields.duration_us && presentation_pts_us != UINT64_MAX &&
        state->previous_picture_pts != UINT64_MAX &&
        presentation_pts_us > state->previous_picture_pts)
    {
        const uint64_t step = presentation_pts_us - state->previous_picture_pts;
        if (step >= 2000 && step <= 1000000)
            fields.duration_us = (uint32_t)(step / 2);
    }
    state->previous_picture_pts = presentation_pts_us;
    if (output->picture_count == 2 && (!fields.first || fields.field_picture))
    {
        (void)pending_pts_drop_second_field(&state->pending_pts);
        state->telemetry.pending_video_timestamps = state->pending_pts.count;
    }

    ++state->telemetry.decoded_frames;
    state->telemetry.last_decoder_output = (uintptr_t)output->buffer;
    state->telemetry.output_pitch = output->pitch;
    state->telemetry.output_surface_height = output->height;
    state->telemetry.decoder_output_in_frame_pool = 1;
    if (!displayable)
    {
        ++state->telemetry.hidden_decoded_frames;
        if (from_drain)
            ++state->telemetry.drained_video_frames;
        return 0;
    }
    const uint64_t first_pts_us = presentation_pts_us;
    const uint32_t field_count =
        !state->config.picture && state->config.bit_depth == 8 && fields.first &&
                fields.duration_us && fields.count >= 2 && first_pts_us != UINT64_MAX &&
                fields.count <= (UINT64_MAX - first_pts_us) / fields.duration_us
            ? fields.count
            : 1;
    state->extra_field_presentations += field_count - 1;
    for (uint32_t field_index = 0; field_index < field_count; ++field_index)
    {
        presentation_pts_us = first_pts_us + (uint64_t)field_index * fields.duration_us;
        result = pace_before_present(state, presentation_pts_us, &drop_frame);
        if (result != 0)
            goto failed;
        if (drop_frame)
            continue;
        result = complete_pending_presentation(state);
        if (result != 0)
            goto failed;

        /* Before the clock below starts: the new interface's build places its
         * own line between that clock and the next statement. */
        if (field_count == 1 && state->config.bit_depth == 8 &&
            (output->picture_count == 2 || g_force_field_blend))
        {
            const uint64_t blend_started = monotonic_us();
            blend_fields(output);
            elapsed = monotonic_us() - blend_started;
            if (elapsed > state->telemetry.decode_max_us)
                state->telemetry.decode_max_us = elapsed;
        }
        started = monotonic_us();
        state->telemetry.last_present_source = (uintptr_t)output->buffer;
        state->telemetry.zero_copy_pointer_match =
            !state->config.picture && !state->telemetry.software_video &&
            state->telemetry.last_decoder_output == state->telemetry.last_present_source;
        rate_now = monotonic_us();
        if (state->controls_started_us == 0)
            state->controls_started_us = rate_now;
        const iptv_native_video_overlay_t overlay = {
            (uint32_t)state->config.codec,
            state->config.visible_width,
            state->config.visible_height,
            state->frame_rate_x100,
            state->bitrate_kbps,
            rate_now - state->controls_started_us < CONTROLS_OVERLAY_US,
            presentation_pts_us,
            field_count > 1 ? 1 + ((fields.first - 1 + field_index) & 1u) : 0,
            state->telemetry.software_video,
            fields.color,
        };
        if (state->config.picture)
        {
            const iptv_native_picture_t picture = {
                output->buffer,          (size_t)output->buffer_size, output->pitch,
                output->height,          state->config.visible_width, state->config.visible_height,
                state->config.bit_depth, presentation_pts_us,         fields.color};
            state->config.picture(state->config.picture_context, &picture);
            result = 0;
        }
        else
            result = iptv_native_agc_present_yuv_deferred(
                output->buffer, (size_t)output->buffer_size, output->pitch, output->height,
                state->config.visible_width, state->config.visible_height, state->config.bit_depth,
                &overlay);
        elapsed = monotonic_us() - started;
        state->telemetry.present_total_us += elapsed;
        if (elapsed > state->telemetry.present_max_us)
            state->telemetry.present_max_us = elapsed;
#if IPTV_PROBE
        if (elapsed > state->probe_present_max_us)
            state->probe_present_max_us = elapsed;
#endif
        if (result != 0)
            goto failed;
        state->pending_present_pts_us = presentation_pts_us;
        state->telemetry.color = fields.color;
        state->telemetry.hdr_output = !state->config.picture && iptv_native_agc_hdr_active();
        state->pause_picture = (iptv_native_picture_t){
            output->buffer,          (size_t)output->buffer_size, output->pitch,
            output->height,          state->config.visible_width, state->config.visible_height,
            state->config.bit_depth, presentation_pts_us,         fields.color};
        state->pause_field = overlay.field;
        state->pending_present_source = output->buffer;
        state->pending_present_from_drain = (uint8_t)(from_drain != 0);
        state->presentation_pending = 1;
        if (state->telemetry.presented_frames == 0)
        {
            result = complete_pending_presentation(state);
            if (result != 0)
                goto failed;
        }
    }
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

static int32_t decode_software_frame(backend_state_t *state, const void *bytes, size_t count,
                                     videodec2_frame_t *frame, videodec2_output_t *output)
{
    /* CPU writes must wait until the previous GPU read has completed. */
    int32_t result = complete_pending_presentation(state);
    if (result != 0)
        return result;
    const uint32_t pitch = state->config.coded_width;
    result = iptv_field_decode(state->field_parser, bytes, count, frame->buffer, frame->buffer_size,
                               pitch, state->config.coded_height, state->config.visible_width,
                               state->config.visible_height);
    if (result < 0)
        return result;
    frame->accepted = 1;
    if (result != 0)
    {
        output->valid = 1;
        output->picture_count = (uint32_t)result;
        output->codec = state->mode->decoder_codec;
        output->width = pitch;
        output->height = state->config.coded_height;
        output->pitch = output->pitch_bytes = pitch;
        output->buffer = frame->buffer;
        output->buffer_size = frame->buffer_size;
    }
    return 0;
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

    state->telemetry.last_video_access_unit_bytes = frame_bytes;
    state->telemetry.last_video_nal_mask = 0;
    if (state->config.codec == IPTV_NATIVE_CODEC_H264)
    {
        const uint8_t *bytes = (const uint8_t *)coded_frame;
        size_t index;
        for (index = 0; index + 4u < frame_bytes; ++index)
        {
            size_t header = 0;
            if (bytes[index] == 0 && bytes[index + 1u] == 0 && bytes[index + 2u] == 1u)
                header = index + 3u;
            else if (index + 4u < frame_bytes && bytes[index] == 0 && bytes[index + 1u] == 0 &&
                     bytes[index + 2u] == 0 && bytes[index + 3u] == 1u)
                header = index + 4u;
            if (header != 0 && header < frame_bytes)
                state->telemetry.last_video_nal_mask |= UINT32_C(1)
                                                        << (bytes[header] & UINT8_C(0x1f));
        }
    }

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
    if (state->presentation_pending && state->pending_present_source == frame_slot)
    {
        result = complete_pending_presentation(state);
        if (result != 0)
            return result;
    }
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

    const iptv_field_info_t fields =
        iptv_field_parse(state->field_parser, coded_frame, frame_bytes);
    started = monotonic_us();
    if (state->telemetry.software_video)
        result = decode_software_frame(state, coded_frame, frame_bytes, &frame, &output);
    else
    {
        result = sceVideodec2Decode(state->decoder, &input, &frame, &output);
        if ((uint32_t)result == UINT32_C(0x811d0303) && fields.first &&
            state->telemetry.submitted_video_access_units == 0 &&
            state->config.coded_width <= 1920 && state->config.coded_height <= 1088)
        {
            /* Valid broadcast pictures can be rejected by VideoDec2 before
             * output. Use the bundled decoder, bounded to 1080i; retain the
             * same field renderer, clocks, controls and ownership. */
            state->telemetry.software_video_trigger = result;
            state->telemetry.software_video = 1;
            memset(&output, 0, sizeof(output));
            output.size = sizeof(output);
            result = decode_software_frame(state, coded_frame, frame_bytes, &frame, &output);
        }
    }
    elapsed = monotonic_us() - started;
    state->telemetry.decode_total_us += elapsed;
    if (elapsed > state->telemetry.decode_max_us)
        state->telemetry.decode_max_us = elapsed;
#if IPTV_PROBE
    if (elapsed > state->probe_decode_max_us)
        state->probe_decode_max_us = elapsed;
#endif
    ++state->telemetry.submitted_video_access_units;
    state->telemetry.submitted_video_bytes += frame_bytes;
    state->telemetry.last_video_pts_us = pts_us;

    if (result != 0 || output.error)
    {
        state->telemetry.decoder_output_valid = output.valid;
        state->telemetry.decoder_output_error = output.error;
        state->telemetry.decoder_output_picture_count = output.picture_count;
        state->telemetry.decoder_frame_accepted = frame.accepted;
        state->telemetry.decoder_output_reject_flags = output.error ? 1u << 1 : 0;
        ++state->telemetry.decoder_errors;
        state->telemetry.last_native_result = result;
        state->telemetry.last_result = result != 0 ? result : IPTV_NATIVE_E_DECODER_OUTPUT;
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
        return state->telemetry.last_result;
    }
    if (!state_pending_push(state, pts_us, displayable, fields))
    {
        ++state->telemetry.decoder_errors;
        state->telemetry.last_result = IPTV_NATIVE_E_DECODER_OUTPUT;
        state->state = IPTV_NATIVE_STATE_ERROR;
        state->telemetry.state = state->state;
        return state->telemetry.last_result;
    }
    if (!output.valid)
    {
        /* Keep VideoDec2's decoded-picture buffer intact. Broadcast H.264 commonly
         * arrives in decode order (I/P before the B pictures displayed ahead of
         * it); flushing here forced that decode order onto the screen. The next
         * Decode call releases pictures in presentation order, paired with the
         * smallest pending PTS below. Flush is reserved for end-of-stream drain. */
        ++state->telemetry.buffered_video_access_units;
        return 0;
    }
    return present_video_output(state, &frame, &output, 1, 0);
}

static int media_span_ready(uint32_t count, uint64_t first, uint64_t last, uint64_t minimum)
{
    // Reject missing/backward timestamps and large discontinuities as buffer duration.
    return count > 1 && last >= first && last - first >= minimum &&
           last - first < UINT64_C(60000000);
}

static int video_queue_at_time_limit(const backend_state_t *state, uint32_t read, uint32_t write)
{
    // Limit demux lead before playback starts too. Keep one second beyond the
    // startup cushion for interleaved audio; downloads/history buffer upstream.
    return write - read > 1 &&
           media_span_ready(
               write - read, atomic_load(&state->video_queue[read % VIDEO_QUEUE_CAPACITY].pts_us),
               atomic_load(&state->video_queue[(write - 1u) % VIDEO_QUEUE_CAPACITY].pts_us),
               DEMUX_BUFFER_US);
}

static int playback_queues_ready(const backend_state_t *state)
{
    // A programme boundary has no more old input to fill a startup buffer.
    if (atomic_load_explicit(&state->programme_draining, memory_order_acquire))
        return 1;
    const uint32_t video_read =
        atomic_load_explicit(&state->video_queue_read, memory_order_acquire);
    const uint32_t video_write =
        atomic_load_explicit(&state->video_queue_write, memory_order_acquire);
    const uint32_t audio_read =
        atomic_load_explicit(&state->audio_queue_read, memory_order_acquire);
    const uint32_t audio_write =
        atomic_load_explicit(&state->audio_queue_write, memory_order_acquire);
    const uint64_t gate_started_us =
        atomic_load_explicit(&state->playback_gate_started_us, memory_order_acquire);
    const uint64_t now = monotonic_us();
    const uint32_t generation = atomic_load(&state->stream_generation);
    const int current_audio =
        audio_write != audio_read && atomic_load(&state->audio_generation) == generation;
    const int timed_out = gate_started_us != 0 && now >= gate_started_us &&
                          now - gate_started_us >= PLAYBACK_START_TIMEOUT_US;
    // Release on queue pressure too: the single demux producer may be blocked
    // before it can deliver the other track. Never wait for an impossible fill.
    const int pressure = video_queue_at_time_limit(state, video_read, video_write) ||
                         video_write - video_read >= VIDEO_QUEUE_CAPACITY - 1u ||
                         (current_audio && audio_write - audio_read >= AUDIO_QUEUE_CAPACITY - 1u) ||
                         atomic_load_explicit(&state->video_queue_bytes, memory_order_acquire) >=
                             VIDEO_QUEUE_MAX_BYTES - INPUT_SLOT_BYTES;
    const int video_ready =
        video_write != video_read &&
        state->video_queue[video_read % VIDEO_QUEUE_CAPACITY].generation == generation &&
        (timed_out || pressure ||
         media_span_ready(
             video_write - video_read,
             atomic_load(&state->video_queue[video_read % VIDEO_QUEUE_CAPACITY].pts_us),
             atomic_load(&state->video_queue[(video_write - 1u) % VIDEO_QUEUE_CAPACITY].pts_us),
             PLAYBACK_BUFFER_US));
    const uint32_t audio_rate = atomic_load(&state->audio_queue_sample_rate);
    const uint32_t audio_type = atomic_load(&state->audio_buffer_type);
    // AAC carries at least 1024 core samples; Layer II carries 1152 per frame.
    // Estimate duration without touching queue storage, which may be released.
    const uint32_t audio_samples = (audio_type == 0x03u || audio_type == 0x04u) ? 1152u
                                   : audio_type == 0x81u                        ? 1536u
                                                                                : 1024u;
    const int audio_ready =
        !audio_type || atomic_load(&state->audio_sync_pending) || timed_out || pressure ||
        (current_audio && audio_rate &&
         (uint64_t)(audio_write - audio_read) * audio_samples * UINT64_C(1000000) / audio_rate >=
             PLAYBACK_BUFFER_US);
    return video_ready && audio_ready;
}

static void restart_playback_buffer(backend_state_t *state)
{
    atomic_store(&state->playback_gate_started_us, monotonic_us());
    atomic_store_explicit(&state->playback_started, 0, memory_order_release);
}

static void release_queued_video(backend_state_t *state)
{
    uint32_t read;
    uint32_t write;

    if (!state->video_queue)
        return;
    read = atomic_load_explicit(&state->video_queue_read, memory_order_relaxed);
    write = atomic_load_explicit(&state->video_queue_write, memory_order_relaxed);
    while (read != write)
    {
        video_queue_item_t *item = &state->video_queue[read % VIDEO_QUEUE_CAPACITY];
        free(item->data);
        item->data = NULL;
        ++read;
    }
    atomic_store_explicit(&state->video_queue_read, write, memory_order_relaxed);
    atomic_store_explicit(&state->video_queue_bytes, 0, memory_order_relaxed);
}

static int32_t pause_video(backend_state_t *state)
{
    if (!atomic_load(&state->paused))
        return 0;
    const uint64_t started = monotonic_us();
    uint64_t redrawn = 0;
    int32_t result = complete_pending_presentation(state);
    if (result)
        return result;
    while (atomic_load(&state->paused) && !atomic_load(&state->stop_requested) &&
           !atomic_load(&state->video_worker_stop))
    {
        const uint64_t now = monotonic_us();
        const iptv_native_picture_t *picture = &state->pause_picture;
        // The decoder is idle on this same thread, so its last picture remains
        // valid. Redraw the OSD on the presenter's copy without counting another
        // decoded/displayed programme frame or changing the subtitle timestamp.
        if (picture->data && !state->config.picture && now - redrawn >= UINT64_C(100000))
        {
            const iptv_native_video_overlay_t overlay = {
                state->config.codec,    picture->width,      picture->height,
                state->frame_rate_x100, state->bitrate_kbps, 0,
                picture->pts_us,        state->pause_field,  state->telemetry.software_video,
                picture->color};
            result = iptv_native_agc_present_yuv_deferred(
                picture->data, picture->bytes, picture->pitch, picture->surface_height,
                picture->width, picture->height, picture->bit_depth, &overlay);
            if (!result)
                result = iptv_native_agc_present_finish_frame();
            if (result)
                return atomic_load(&state->stop_requested) ? 0 : result;
            redrawn = now;
        }
        (void)sceKernelUsleep(5000u);
    }
    if (state->pace_active)
        state->pace_base_clock_us += monotonic_us() - started;
    state->last_present_monotonic_us = 0;
    state->frame_rate_window_frames = 0;
    return 0;
}

static void *video_worker_entry(void *argument)
{
    backend_state_t *state = argument;
    int had_data = 0;
    int empty_reported = 0;
    uint64_t starved_since_us = 0;

    for (;;)
    {
        if (atomic_load(&state->paused))
        {
            const int32_t result = pause_video(state);
            starved_since_us = 0;
            if (result)
            {
                atomic_store(&state->video_worker_result, result);
                break;
            }
        }
        if (atomic_load_explicit(&state->playback_started, memory_order_acquire) &&
            !atomic_load(&state->video_worker_stop) && !atomic_load(&state->stop_requested) &&
            !atomic_load(&state->programme_draining))
        {
            const int empty =
                atomic_load(&state->video_queue_read) == atomic_load(&state->video_queue_write) ||
                (atomic_load(&state->audio_buffer_type) &&
                 !atomic_load(&state->audio_sync_pending) &&
                 !atomic_load(&state->audio_worker_stop) &&
                 atomic_load(&state->audio_worker_result) == 0 &&
                 atomic_load(&state->audio_queue_read) == atomic_load(&state->audio_queue_write));
            const uint64_t now = monotonic_us();
            if (!empty)
                starved_since_us = 0;
            else if (!starved_since_us)
                starved_since_us = now;
            else if (now - starved_since_us >= PLAYBACK_UNDERRUN_GRACE_US)
            {
                restart_playback_buffer(state);
                starved_since_us = 0;
            }
        }
        const uint32_t read = atomic_load_explicit(&state->video_queue_read, memory_order_relaxed);
        const uint32_t write =
            atomic_load_explicit(&state->video_queue_write, memory_order_acquire);
        if (read == write)
        {
            if (atomic_load_explicit(&state->video_worker_stop, memory_order_acquire) ||
                atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
                break;
            if (had_data && !empty_reported && atomic_load(&state->playback_started) &&
                !atomic_load(&state->programme_draining))
            {
                ++state->telemetry.video_queue_underruns;
                empty_reported = 1;
            }
            (void)sceKernelUsleep(1000u);
            continue;
        }

        video_queue_item_t *item = &state->video_queue[read % VIDEO_QUEUE_CAPACITY];
        const uint32_t bytes = item->bytes;
        const uint32_t generation =
            atomic_load_explicit(&state->stream_generation, memory_order_acquire);
        if (item->generation != generation)
        {
            free(item->data);
            item->data = NULL;
            atomic_fetch_sub_explicit(&state->video_queue_bytes, bytes, memory_order_release);
            atomic_store_explicit(&state->video_queue_read, read + 1u, memory_order_release);
            ++state->telemetry.dropped_delayed_frames;
            continue;
        }
        if (state->video_generation != generation)
        {
            const int32_t completed = complete_pending_presentation(state);
            if (completed != 0)
            {
                atomic_store_explicit(&state->video_worker_result, completed, memory_order_release);
                break;
            }
            int32_t reset = 0;
            if (state->telemetry.software_video)
                iptv_field_decoder_reset(state->field_parser);
            else
                reset = sceVideodec2Reset(state->decoder);
            state->previous_picture_pts = UINT64_MAX;
            if (reset != 0)
            {
                atomic_store_explicit(&state->video_worker_result, reset, memory_order_release);
                break;
            }
            discard_pending_video(state);
            state->pause_picture = (iptv_native_picture_t){0};
            state->pace_active = 0;
            state->video_generation = generation;
            // Old entries must be discarded before waiting for fresh media.
            // A seek to live needs the same cushion as initial playback.
            restart_playback_buffer(state);
            starved_since_us = 0;
        }
        if (!atomic_load_explicit(&state->playback_started, memory_order_acquire) &&
            !atomic_load_explicit(&state->video_worker_stop, memory_order_acquire) &&
            !atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
        {
            if (!playback_queues_ready(state))
            {
                (void)sceKernelUsleep(1000u);
                continue;
            }
            state->pace_active = 0;
            atomic_store_explicit(&state->playback_started, 1, memory_order_release);
        }
        const int32_t result =
            submit_coded_frame(state, item->data, bytes, item->pts_us, item->displayable);
        free(item->data);
        item->data = NULL;
        atomic_fetch_sub_explicit(&state->video_queue_bytes, bytes, memory_order_release);
        atomic_store_explicit(&state->video_queue_read, read + 1u, memory_order_release);
        had_data = 1;
        empty_reported = 0;
        if (result != 0)
        {
            if (result == IPTV_NATIVE_E_CANCELLED &&
                atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
                break;
            atomic_store_explicit(&state->video_worker_result, result, memory_order_release);
            break;
        }
    }
    atomic_store_explicit(&state->video_worker_done, 1, memory_order_release);
    return NULL;
}

static int32_t start_video_worker(backend_state_t *state)
{
    int32_t result;

    state->video_queue = calloc(VIDEO_QUEUE_CAPACITY, sizeof(*state->video_queue));
    if (!state->video_queue)
        return IPTV_NATIVE_E_STATE;
    atomic_store_explicit(&state->video_queue_read, 0, memory_order_relaxed);
    atomic_store_explicit(&state->video_queue_write, 0, memory_order_relaxed);
    atomic_store_explicit(&state->video_queue_bytes, 0, memory_order_relaxed);
    atomic_store_explicit(&state->video_worker_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&state->video_worker_done, 0, memory_order_relaxed);
    atomic_store_explicit(&state->video_worker_result, 0, memory_order_relaxed);
    result =
        scePthreadCreate(&state->video_thread, NULL, video_worker_entry, state, "prosperotv-video");
    if (result != 0)
    {
        free(state->video_queue);
        state->video_queue = NULL;
    }
    return result;
}

static int32_t stop_video_worker(backend_state_t *state)
{
    int32_t result = 0;

    if (state->video_thread)
    {
        void *thread_result = NULL;
        atomic_store_explicit(&state->playback_started, 1, memory_order_release);
        atomic_store_explicit(&state->video_worker_stop, 1, memory_order_release);
        while (!atomic_load_explicit(&state->video_worker_done, memory_order_acquire))
        {
            if (state->config.poll_controls)
                state->config.poll_controls(state->config.controls_context);
            (void)sceKernelUsleep(1000u);
        }
        result = scePthreadJoin(state->video_thread, &thread_result);
        state->video_thread = NULL;
    }
    if (result == 0)
        result = atomic_load_explicit(&state->video_worker_result, memory_order_acquire);
    release_queued_video(state);
    free(state->video_queue);
    state->video_queue = NULL;
    return result;
}

static int32_t queue_coded_frame(backend_state_t *state, const void *coded_frame,
                                 size_t frame_bytes, uint64_t pts_us, int displayable)
{
    uint8_t *copy;
    uint32_t read;
    uint32_t write;
    uint64_t queued_bytes;
    const uint32_t generation =
        atomic_load_explicit(&state->stream_generation, memory_order_acquire);

    for (;;)
    {
        if (atomic_load_explicit(&state->discard_input, memory_order_acquire))
            return 0;
        const int32_t worker_result =
            atomic_load_explicit(&state->video_worker_result, memory_order_acquire);
        if (worker_result != 0)
            return worker_result;
        if (state->config.picture_cancelled &&
            state->config.picture_cancelled(state->config.picture_context))
            return IPTV_NATIVE_E_CANCELLED;
        if (atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
            return 0;
        read = atomic_load_explicit(&state->video_queue_read, memory_order_acquire);
        write = atomic_load_explicit(&state->video_queue_write, memory_order_relaxed);
        queued_bytes = atomic_load_explicit(&state->video_queue_bytes, memory_order_acquire);
        if (write - read < VIDEO_QUEUE_CAPACITY && !video_queue_at_time_limit(state, read, write) &&
            (queued_bytes == 0 || frame_bytes <= VIDEO_QUEUE_MAX_BYTES - queued_bytes))
            break;
        (void)sceKernelUsleep(1000u);
    }

    copy = malloc(frame_bytes);
    if (!copy)
        return IPTV_NATIVE_E_STATE;
    memcpy(copy, coded_frame, frame_bytes);
    video_queue_item_t *item = &state->video_queue[write % VIDEO_QUEUE_CAPACITY];
    item->pts_us = pts_us;
    item->bytes = (uint32_t)frame_bytes;
    item->generation = generation;
    item->displayable = displayable ? 1u : 0u;
    item->data = copy;
    queued_bytes =
        atomic_fetch_add_explicit(&state->video_queue_bytes, frame_bytes, memory_order_release) +
        frame_bytes;
    atomic_store_explicit(&state->video_queue_write, write + 1u, memory_order_release);
    if (write - read + 1u > state->telemetry.video_queue_max_frames)
        state->telemetry.video_queue_max_frames = write - read + 1u;
    if (queued_bytes > state->telemetry.video_queue_max_bytes)
        state->telemetry.video_queue_max_bytes = queued_bytes;
    return 0;
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
        return 0;
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
        record_media_bytes(state, access_unit_bytes);
        return queue_coded_frame(state, coded_packet, access_unit_bytes, pts_us, 1);
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
    record_media_bytes(state, access_unit_bytes);
    for (frame_index = 0; frame_index < vp9_packet.count; ++frame_index)
    {
        int32_t result = queue_coded_frame(state, vp9_packet.frames[frame_index].data,
                                           vp9_packet.frames[frame_index].bytes, pts_us,
                                           vp9_flags[frame_index].displayable);
        if (result != 0)
            return result;
    }
    return 0;
}

static int32_t decode_software_audio(backend_state_t *state, const uint8_t *data, size_t bytes)
{
    if (!state->software_audio)
        state->software_audio = iptv_audio_decoder_create(state->config.audio_stream_type);
    if (!state->software_audio)
        return IPTV_NATIVE_E_UNSUPPORTED;
    uint32_t rate = 0;
    const int result =
        iptv_audio_decode(state->software_audio, data, bytes, (int16_t *)state->audio_pcm,
                          sizeof(state->audio_pcm), &rate);
    if (result < 0)
        return result;
    state->audio_pcm_item.length = (uint32_t)result;
    state->audio_info.channel_count = 2;
    state->audio_info.sampling_frequency = rate;
    return 0;
}

static int32_t decode_audio_frame(backend_state_t *state, const void *adts_frame,
                                  size_t frame_bytes, uint64_t pts_us)
{
    const uint8_t *adts = adts_frame;
    uint32_t pcm_rate;
    int32_t result;

    if (!state || !adts_frame)
        return IPTV_NATIVE_E_ARGUMENT;

    const int mp2 =
        state->config.audio_stream_type == 0x03u || state->config.audio_stream_type == 0x04u;
    if (state->software_audio || iptv_audio_software_type(state->config.audio_stream_type) ||
        (!mp2 && (frame_bytes < 7u || (adts[2] >> 6) != 1u ||
                  adts_channels(adts, frame_bytes) == 0 || adts_channels(adts, frame_bytes) > 2)))
    {
        result = decode_software_audio(state, adts, frame_bytes);
    }
    else if (mp2)
    {
        mp3dec_frame_info_t info = {0};
        const int samples = mp3dec_decode_frame(&state->mp2_decoder, adts, (int)frame_bytes,
                                                (mp3d_sample_t *)state->audio_pcm, &info);
        if (samples != 1152 || info.layer != 2 || info.frame_offset != 0 ||
            info.frame_bytes != (int)frame_bytes || info.channels < 1 || info.channels > 2 ||
            info.hz < 16000 || info.hz > 48000)
            return IPTV_NATIVE_E_AUDIO_FRAME;
        state->audio_pcm_item.length =
            (uint32_t)samples * (uint32_t)info.channels * sizeof(int16_t);
        state->audio_info.channel_count = (uint32_t)info.channels;
        state->audio_info.sampling_frequency = (uint32_t)info.hz;
        result = 0;
    }
    else
    {
        state->audio_au.address = (void *)adts_frame;
        state->audio_au.length = (uint32_t)frame_bytes;
        state->audio_pcm_item.address = state->audio_pcm;
        state->audio_pcm_item.length = sizeof(state->audio_pcm);
        result = sceAudiodecDecode(state->audio_decoder, &state->audio_ctrl);
        if (result < 0)
            result = decode_software_audio(state, adts, frame_bytes);
    }
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

    pcm_rate =
        (mp2 || state->software_audio)
            ? state->audio_info.sampling_frequency
            : decoded_pcm_rate(adts, frame_bytes, state->audio_info.channel_count,
                               state->audio_pcm_item.length, state->audio_info.sampling_frequency);
    if (state->audio_sink.handle < 0)
    {
        if (state->audio_staged_bytes == 0)
        {
            memcpy(state->audio_staged_pcm, state->audio_pcm, state->audio_pcm_item.length);
            state->audio_staged_pts_us = pts_us;
            state->audio_staged_bytes = state->audio_pcm_item.length;
            state->audio_staged_rate = pcm_rate;
            state->audio_staged_channels = state->audio_info.channel_count;
            ++state->telemetry.decoded_audio_frames;
            return 0;
        }
        if (state->audio_staged_channels != state->audio_info.channel_count)
        {
            result = IPTV_NATIVE_E_AUDIO_FRAME;
            goto failed;
        }
        if (!state->software_audio)
            pcm_rate =
                pcm_rate_from_pts(state->audio_staged_bytes, state->audio_staged_channels,
                                  state->audio_staged_pts_us, pts_us, state->audio_staged_rate);
        result = audio_sink_open(state, pcm_rate, state->audio_info.channel_count);
        if (result < 0)
            goto failed;
        {
            const uint32_t sample_count = state->audio_staged_bytes / sizeof(int16_t);
            state->audio_staged_bytes = 0;
            result = audio_push_pcm(state, (const int16_t *)state->audio_staged_pcm, sample_count);
            if (result < 0)
                goto failed;
        }
    }
    else if (state->audio_sink.channels != state->audio_info.channel_count)
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
    return result;
}

// Positive waits, negative discards an old audio frame, zero starts playback.
static int audio_sync_action(const backend_state_t *state, uint64_t pts, uint64_t waited_us)
{
    if (atomic_load(&state->audio_worker_stop) || waited_us >= UINT64_C(30000000))
        return 0;
    // A rewind's audio cannot align against the picture from before the seek.
    if (atomic_load_explicit(&state->presented_generation, memory_order_acquire) !=
        atomic_load_explicit(&state->stream_generation, memory_order_acquire))
        return 1;
    const uint64_t video_pts = atomic_load_explicit(&state->presented_pts_us, memory_order_acquire);
    if (pts == UINT64_MAX || video_pts == UINT64_MAX)
        return 0;
    if (pts > video_pts && pts - video_pts > UINT64_C(50000))
        return 1;
    if (video_pts > pts && video_pts - pts > UINT64_C(100000))
        return -1;
    return 0;
}

static void *audio_worker_entry(void *argument)
{
    backend_state_t *state = argument;
    int had_data = 0;
    int empty_reported = 0;
    uint64_t sync_started = 0;

    for (;;)
    {
        if (atomic_load_explicit(&state->stop_requested, memory_order_relaxed) ||
            atomic_load_explicit(&state->audio_worker_discard, memory_order_acquire))
            break;
        if (atomic_load(&state->paused) && !atomic_load(&state->audio_worker_stop))
        {
            sync_started = 0;
            (void)sceKernelUsleep(5000u);
            continue;
        }
        const uint32_t read = atomic_load_explicit(&state->audio_queue_read, memory_order_relaxed);
        const uint32_t write =
            atomic_load_explicit(&state->audio_queue_write, memory_order_acquire);
        if (read == write)
        {
            if (atomic_load_explicit(&state->audio_worker_stop, memory_order_acquire))
                break;
            if (had_data && !empty_reported && atomic_load(&state->playback_started) &&
                !atomic_load(&state->programme_draining))
            {
                ++state->telemetry.audio_queue_underruns;
                empty_reported = 1;
            }
            (void)sceKernelUsleep(1000u);
            continue;
        }

        const audio_queue_item_t *item = &state->audio_queue[read % AUDIO_QUEUE_CAPACITY];
        const uint32_t generation =
            atomic_load_explicit(&state->stream_generation, memory_order_acquire);
        if (item->generation != generation)
        {
            atomic_store_explicit(&state->audio_queue_read, read + 1u, memory_order_release);
            continue;
        }
        if (state->audio_generation != generation)
        {
            const int32_t reset = reset_native_audio_decoder(state);
            if (reset < 0)
            {
                atomic_store_explicit(&state->audio_worker_result, reset, memory_order_release);
                break;
            }
            mp3dec_init(&state->mp2_decoder);
            iptv_audio_decoder_reset(state->software_audio);
            state->audio_staged_bytes = 0;
            state->audio_sink.pending = 0;
            state->audio_sink.have_previous = 0;
            state->audio_sink.input_index = 0;
            state->audio_sink.next_output_position = 0;
            state->audio_generation = generation;
            sync_started = 0;
        }
        if (!atomic_load_explicit(&state->playback_started, memory_order_acquire) &&
            !atomic_load_explicit(&state->programme_draining, memory_order_acquire) &&
            !atomic_load_explicit(&state->audio_worker_stop, memory_order_acquire))
        {
            // Drain stale entries before waiting, so they cannot fill the queue
            // and block the producer from delivering the new video's buffer.
            (void)sceKernelUsleep(1000u);
            continue;
        }
        if (atomic_load(&state->audio_sync_pending))
        {
            const uint64_t now = monotonic_us();
            if (!sync_started)
                sync_started = now;
            // Track changes reach the demuxer ahead of the displayed picture.
            // Wait for video to catch up, bounded for broken/missing timestamps.
            const int sync = audio_sync_action(state, item->pts_us, now - sync_started);
            if (sync > 0)
            {
                (void)sceKernelUsleep(1000u);
                continue;
            }
            if (sync < 0)
            {
                atomic_store_explicit(&state->audio_queue_read, read + 1u, memory_order_release);
                continue;
            }
            atomic_store(&state->audio_sync_pending, 0);
        }
        const int32_t result = decode_audio_frame(state, item->data, item->bytes, item->pts_us);
        atomic_store_explicit(&state->audio_queue_read, read + 1u, memory_order_release);
        had_data = 1;
        empty_reported = 0;
        if (result != 0)
        {
            atomic_store_explicit(&state->audio_worker_result, result, memory_order_release);
            break;
        }
    }
    return NULL;
}

static int32_t start_audio_worker(backend_state_t *state)
{
    int32_t result;

    state->audio_queue = calloc(AUDIO_QUEUE_CAPACITY, sizeof(*state->audio_queue));
    if (!state->audio_queue)
        return IPTV_NATIVE_E_STATE;
    atomic_store_explicit(&state->audio_queue_read, 0, memory_order_relaxed);
    atomic_store_explicit(&state->audio_queue_write, 0, memory_order_relaxed);
    atomic_store_explicit(&state->audio_worker_stop, 0, memory_order_relaxed);
    atomic_store_explicit(&state->audio_worker_discard, 0, memory_order_relaxed);
    atomic_store_explicit(&state->audio_worker_result, 0, memory_order_relaxed);
    result =
        scePthreadCreate(&state->audio_thread, NULL, audio_worker_entry, state, "prosperotv-audio");
    if (result != 0)
    {
        free(state->audio_queue);
        state->audio_queue = NULL;
    }
    return result;
}

static int32_t stop_audio_worker(backend_state_t *state)
{
    int32_t result = 0;

    if (state->audio_thread)
    {
        void *thread_result = NULL;
        atomic_store_explicit(&state->audio_worker_stop, 1, memory_order_release);
        result = scePthreadJoin(state->audio_thread, &thread_result);
        state->audio_thread = NULL;
    }
    if (atomic_load_explicit(&state->audio_worker_result, memory_order_acquire) != 0)
    {
        state->telemetry.last_audio_result =
            atomic_load_explicit(&state->audio_worker_result, memory_order_relaxed);
        state->telemetry.audio_disabled = 1;
        state->config.enable_audio = 0;
        atomic_store(&state->audio_buffer_type, 0);
    }
    free(state->audio_queue);
    state->audio_queue = NULL;
    return result;
}

int32_t iptv_native_backend_submit_audio(iptv_native_backend_t *backend, const void *adts_frame,
                                         size_t frame_bytes, uint64_t pts_us)
{
    backend_state_t *state = state_from(backend);
    const uint8_t *adts = adts_frame;
    size_t declared_bytes;
    uint32_t channels;
    uint32_t rate = 0;
    uint32_t read;
    uint32_t write;

    if (!state || state->magic != BACKEND_MAGIC || !adts_frame)
        return IPTV_NATIVE_E_ARGUMENT;
    if (state->state != IPTV_NATIVE_STATE_OPEN || state->drain_started)
        return IPTV_NATIVE_E_STATE;
    const uint32_t generation =
        atomic_load_explicit(&state->stream_generation, memory_order_acquire);
    if (atomic_load_explicit(&state->discard_input, memory_order_acquire))
        return 0;
    const int mp2 =
        state->config.audio_stream_type == 0x03u || state->config.audio_stream_type == 0x04u;
    if (!state->config.enable_audio)
        return 0;
    if (atomic_load_explicit(&state->audio_worker_result, memory_order_acquire) != 0)
    {
        const int32_t error =
            atomic_load_explicit(&state->audio_worker_result, memory_order_relaxed);
        (void)disable_audio_internal(state, error);
        return error;
    }
    if (atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
        return 0;
    if (iptv_audio_software_type(state->config.audio_stream_type))
    {
        uint32_t samples = 0;
        declared_bytes = iptv_audio_frame_info(state->config.audio_stream_type, adts, frame_bytes,
                                               &rate, &samples);
        if (!declared_bytes || declared_bytes != frame_bytes || frame_bytes > AUDIO_FRAME_MAX_BYTES)
        {
            (void)disable_audio_internal(state, IPTV_NATIVE_E_AUDIO_FRAME);
            return IPTV_NATIVE_E_AUDIO_FRAME;
        }
        // LATM carries its rate inside AudioSpecificConfig; only the buffering estimate
        // uses 48 kHz here. PCM playback uses the decoder's actual sample rate.
        if (!rate)
            rate = 48000;
    }
    else if (mp2)
    {
        declared_bytes = iptv_mp2_frame_info(adts, frame_bytes, &rate, &channels);
        if (!declared_bytes || declared_bytes != frame_bytes || frame_bytes > AUDIO_FRAME_MAX_BYTES)
        {
            (void)disable_audio_internal(state, IPTV_NATIVE_E_AUDIO_FRAME);
            return IPTV_NATIVE_E_AUDIO_FRAME;
        }
    }
    else
    {
        if (frame_bytes < 7 || frame_bytes > AUDIO_FRAME_MAX_BYTES || adts[0] != 0xffu ||
            (adts[1] & 0xf6u) != 0xf0u)
        {
            (void)disable_audio_internal(state, IPTV_NATIVE_E_AUDIO_FRAME);
            return IPTV_NATIVE_E_AUDIO_FRAME;
        }
        declared_bytes =
            ((size_t)(adts[3] & 3u) << 11) | ((size_t)adts[4] << 3) | ((size_t)adts[5] >> 5);
        channels = adts_channels(adts, frame_bytes);
        if (declared_bytes != frame_bytes || adts_core_rate(adts, frame_bytes) == 0)
        {
            (void)disable_audio_internal(state, IPTV_NATIVE_E_AUDIO_FRAME);
            return IPTV_NATIVE_E_AUDIO_FRAME;
        }
        rate = adts_core_rate(adts, frame_bytes);
    }
    record_media_bytes(state, frame_bytes);

    for (;;)
    {
        if (atomic_load_explicit(&state->discard_input, memory_order_acquire))
            return 0;
        read = atomic_load_explicit(&state->audio_queue_read, memory_order_acquire);
        write = atomic_load_explicit(&state->audio_queue_write, memory_order_relaxed);
        if (write - read < AUDIO_QUEUE_CAPACITY)
            break;
        if (atomic_load_explicit(&state->stop_requested, memory_order_relaxed))
            return 0;
        if (atomic_load_explicit(&state->audio_worker_result, memory_order_acquire) != 0)
            return atomic_load_explicit(&state->audio_worker_result, memory_order_relaxed);
        (void)sceKernelUsleep(1000u);
    }

    audio_queue_item_t *item = &state->audio_queue[write % AUDIO_QUEUE_CAPACITY];
    atomic_store(&state->audio_queue_sample_rate, rate);
    item->pts_us = pts_us;
    item->bytes = (uint32_t)frame_bytes;
    item->generation = generation;
    memcpy(item->data, adts_frame, frame_bytes);
    atomic_store_explicit(&state->audio_queue_write, write + 1u, memory_order_release);
    if (write - read + 1u > state->telemetry.audio_queue_max_frames)
        state->telemetry.audio_queue_max_frames = write - read + 1u;
    return 0;
}

int32_t iptv_native_backend_disable_audio(iptv_native_backend_t *backend)
{
    backend_state_t *state = state_from(backend);

    if (!state || state->magic != BACKEND_MAGIC)
        return IPTV_NATIVE_E_ARGUMENT;
    if (state->state != IPTV_NATIVE_STATE_OPEN || state->drain_started)
        return IPTV_NATIVE_E_STATE;
    if (!state->config.enable_audio)
        return 0;
    return disable_audio_internal(state, IPTV_NATIVE_E_AUDIO_FRAME);
}

int32_t iptv_native_backend_programme_boundary(iptv_native_backend_t *backend)
{
    backend_state_t *state = state_from(backend);
    if (!state || state->magic != BACKEND_MAGIC)
        return IPTV_NATIVE_E_ARGUMENT;
    if (state->state != IPTV_NATIVE_STATE_OPEN || state->drain_started)
        return IPTV_NATIVE_E_STATE;
    int32_t result = 0;
    uint64_t started = monotonic_us();
    atomic_store_explicit(&state->programme_draining, 1, memory_order_release);
    while (atomic_load(&state->video_queue_read) != atomic_load(&state->video_queue_write) ||
           (atomic_load(&state->audio_buffer_type) &&
            atomic_load(&state->audio_queue_read) != atomic_load(&state->audio_queue_write)))
    {
        // The main/control thread remains active while this demux thread waits.
        // A seek or stop discards this boundary; a pause retains it until resume.
        if (atomic_load(&state->discard_input) || atomic_load(&state->stop_requested))
            break;
        result = atomic_load(&state->video_worker_result);
        if (!result && atomic_load(&state->audio_buffer_type))
            result = atomic_load(&state->audio_worker_result);
        if (result)
            break;
        const uint64_t now = monotonic_us();
        if (atomic_load(&state->paused))
            started = now;
        else if (now - started > UINT64_C(30000000))
        {
            result = IPTV_NATIVE_E_STATE;
            break;
        }
        (void)sceKernelUsleep(1000u);
    }
    atomic_store_explicit(&state->programme_draining, 0, memory_order_release);
    return result;
}

int32_t iptv_native_backend_select_audio(iptv_native_backend_t *backend, uint32_t stream_type)
{
    backend_state_t *state = state_from(backend);
    if (!state || state->magic != BACKEND_MAGIC)
        return IPTV_NATIVE_E_ARGUMENT;
    if (state->state != IPTV_NATIVE_STATE_OPEN || state->drain_started)
        return IPTV_NATIVE_E_STATE;
    if (stream_type && stream_type != 0x0fu && stream_type != 0x03u && stream_type != 0x04u &&
        !iptv_audio_software_type(stream_type))
        return IPTV_NATIVE_E_UNSUPPORTED;

    atomic_store(&state->audio_buffer_type, 0);
    atomic_store_explicit(&state->audio_worker_discard, 1, memory_order_release);
    const int32_t cleanup = release_audio(state);
    if (cleanup && !state->telemetry.cleanup_result)
        state->telemetry.cleanup_result = cleanup;
    memset(&state->audio_sink, 0, sizeof(state->audio_sink));
    state->audio_sink.handle = -1;
    atomic_store_explicit(&state->audio_queue_sample_rate, 0, memory_order_relaxed);
    state->config.enable_audio = stream_type != 0;
    state->config.audio_stream_type = stream_type;
    state->audio_generation = atomic_load_explicit(&state->stream_generation, memory_order_acquire);
    atomic_store(&state->audio_sync_pending, stream_type != 0);
    atomic_store(&state->audio_buffer_type, stream_type);
    state->telemetry.audio_disabled = stream_type == 0;
    state->telemetry.last_audio_result = 0;
    if (!stream_type)
        return cleanup;
    int32_t result = initialize_audio(state);
    if (!result)
        result = start_audio_worker(state);
    if (result)
        (void)disable_audio_internal(state, result);
    return result;
}

int32_t iptv_native_backend_discontinuity(iptv_native_backend_t *backend)
{
    backend_state_t *state = state_from(backend);

    if (!state || state->magic != BACKEND_MAGIC)
        return IPTV_NATIVE_E_ARGUMENT;
    if (state->state != IPTV_NATIVE_STATE_OPEN || state->drain_started)
        return IPTV_NATIVE_E_STATE;
    atomic_fetch_add_explicit(&state->stream_generation, 1u, memory_order_acq_rel);
    atomic_store(&state->audio_sync_pending, state->config.enable_audio != 0);
    atomic_store_explicit(&state->discard_input, 0, memory_order_release);
    return 0;
}

void iptv_native_backend_set_paused(iptv_native_backend_t *backend, int paused)
{
    backend_state_t *state = state_from(backend);
    if (state && state->magic == BACKEND_MAGIC)
        atomic_store_explicit(&state->paused, paused != 0, memory_order_release);
}
int iptv_native_backend_paused(const iptv_native_backend_t *backend)
{
    const backend_state_t *state = const_state_from(backend);
    return state && state->magic == BACKEND_MAGIC && atomic_load(&state->paused);
}
void iptv_native_backend_request_reposition(iptv_native_backend_t *backend)
{
    backend_state_t *state = state_from(backend);
    if (state && state->magic == BACKEND_MAGIC && state->state == IPTV_NATIVE_STATE_OPEN)
    {
        atomic_store_explicit(&state->discard_input, 1, memory_order_release);
        atomic_fetch_add_explicit(&state->stream_generation, 1u, memory_order_acq_rel);
        atomic_store_explicit(&state->paused, 0, memory_order_release);
    }
}
uint64_t iptv_native_backend_presented_pts(const iptv_native_backend_t *backend)
{
    const backend_state_t *state = const_state_from(backend);
    if (!state || state->magic != BACKEND_MAGIC)
        return UINT64_MAX;
    const uint32_t generation =
        atomic_load_explicit(&state->stream_generation, memory_order_acquire);
    if (atomic_load_explicit(&state->discard_input, memory_order_acquire) ||
        atomic_load_explicit(&state->presented_generation, memory_order_acquire) != generation)
        return UINT64_MAX;
    const uint64_t pts = atomic_load_explicit(&state->presented_pts_us, memory_order_acquire);
    return atomic_load_explicit(&state->stream_generation, memory_order_acquire) == generation &&
                   !atomic_load_explicit(&state->discard_input, memory_order_acquire)
               ? pts
               : UINT64_MAX;
}

void iptv_native_backend_request_stop(iptv_native_backend_t *backend)
{
    backend_state_t *state = state_from(backend);

    if (!state || state->magic != BACKEND_MAGIC)
        return;
    atomic_store_explicit(&state->stop_requested, 1, memory_order_relaxed);
    atomic_store_explicit(&state->audio_worker_stop, 1, memory_order_release);
    atomic_store_explicit(&state->video_worker_stop, 1, memory_order_release);
    state->telemetry.stop_requested = 1;
    if (!state->config.picture)
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
        result = state->telemetry.software_video
                     ? decode_software_frame(state, NULL, 0, &frame, &output)
                     : sceVideodec2Flush(state->decoder, &frame, &output);
        elapsed = monotonic_us() - started;
        state->telemetry.decode_total_us += elapsed;
        if (elapsed > state->telemetry.decode_max_us)
            state->telemetry.decode_max_us = elapsed;
#if IPTV_PROBE
        if (elapsed > state->probe_decode_max_us)
            state->probe_decode_max_us = elapsed;
#endif
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
    first_result = stop_video_worker(state);
    result = drain_video(state);
    if (first_result == 0 && result != 0)
        first_result = result;
    result = complete_pending_presentation(state);
    if (first_result == 0 && result != 0)
        first_result = result;
    result = audio_drain(state);
    if (first_result == 0 && result != 0)
        first_result = result;
    result = state->config.picture ? 0 : iptv_native_agc_present_drain();
    if (first_result == 0 && result != 0)
        first_result = result;
    if (first_result == 0 &&
        (state->telemetry.hardware_validated || state->telemetry.software_video) &&
        state->telemetry.decoded_frames != 0 &&
        state->telemetry.presented_frames + state->telemetry.hidden_decoded_frames +
                state->telemetry.dropped_late_video_frames ==
            state->telemetry.decoded_frames + state->extra_field_presentations &&
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

uint64_t iptv_native_backend_presented_frames(const iptv_native_backend_t *backend)
{
    const backend_state_t *state = const_state_from(backend);

    return state && state->magic == BACKEND_MAGIC
               ? atomic_load_explicit(&state->presented_frame_count, memory_order_acquire)
               : 0;
}

int32_t iptv_native_backend_close(iptv_native_backend_t *backend)
{
    backend_state_t *state = state_from(backend);
    int32_t first_result;
    int32_t result;

    if (!state || state->magic != BACKEND_MAGIC)
        return IPTV_NATIVE_E_ARGUMENT;
    if (state->state == IPTV_NATIVE_STATE_CLOSED || state->state == IPTV_NATIVE_STATE_IDLE)
        return 0;
    first_result = state->telemetry.cleanup_result;
    result = iptv_native_backend_drain(backend);
    /* Cancelling a pending picture is normal when closing a preview. Keep
     * subsequent resource-release failures visible instead of masking them. */
    if (first_result == 0 && result != 0 && result != IPTV_NATIVE_E_CANCELLED)
        first_result = result;
    state->state = IPTV_NATIVE_STATE_STOPPING;
    state->telemetry.state = state->state;
    iptv_native_backend_request_stop(backend);
    result = state->config.picture ? 0 : iptv_native_agc_present_shutdown();
    if (first_result == 0 && result != 0)
        first_result = result;

    result = release_audio(state);
    if (first_result == 0 && result != 0)
        first_result = result;

    iptv_field_parser_destroy(state->field_parser);
    state->field_parser = NULL;
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
static int test_volume_calls, test_volume_left, test_volume_right;
static int test_control_polls, test_join_calls;
static int test_audio_creates, test_audio_deletes, test_audio_deleted_handle;
static int test_audio_create_result = 51, test_audio_delete_result;
static iptv_native_backend_t *test_reposition_on_sleep;
static backend_state_t *test_programme_drain_on_sleep;
static unsigned test_programme_drain_steps;
int sceAudiodecCreateDecoder(sce_audiodec_ctrl_t *ctrl, uint32_t codec_type)
{
    assert(codec_type == AUDIODEC_AAC);
    assert(ctrl->param && ctrl->stream_info && ctrl->au_info && ctrl->pcm_item);
    assert(!ctrl->au_info->address && !ctrl->au_info->length);
    assert(!ctrl->pcm_item->address && !ctrl->pcm_item->length);
    ++test_audio_creates;
    return test_audio_create_result;
}
int sceAudiodecDeleteDecoder(int handle)
{
    test_audio_deleted_handle = handle;
    ++test_audio_deletes;
    return test_audio_delete_result;
}
int sceKernelUsleep(uint32_t microseconds)
{
    (void)microseconds;
    if (test_programme_drain_on_sleep)
    {
        backend_state_t *state = test_programme_drain_on_sleep;
        assert(atomic_load(&state->programme_draining));
        assert(playback_queues_ready(state));
        if (++test_programme_drain_steps == 1)
            atomic_store(&state->video_queue_read, atomic_load(&state->video_queue_write));
        else
        {
            atomic_store(&state->audio_queue_read, atomic_load(&state->audio_queue_write));
            test_programme_drain_on_sleep = NULL;
        }
    }
    if (test_reposition_on_sleep)
    {
        iptv_native_backend_request_reposition(test_reposition_on_sleep);
        test_reposition_on_sleep = NULL;
    }
    return 0;
}
int scePthreadJoin(void *thread, void **result)
{
    (void)result;
    backend_state_t *state = thread;
    assert(atomic_load(&state->video_worker_done));
    ++test_join_calls;
    return 0;
}
static void test_drain_controls(void *context)
{
    backend_state_t *state = context;
    if (++test_control_polls == 3)
    {
        // Simulate a decoder waiting on queued playback until a user stops it.
        atomic_store(&state->stop_requested, 1);
        atomic_store(&state->video_worker_done, 1);
    }
}
int sceAudioOutSetVolume(int handle, int flags, const int *volumes)
{
    assert(handle == 7 && flags == 3);
    ++test_volume_calls;
    test_volume_left = volumes[0];
    test_volume_right = volumes[1];
    return 0;
}

int main(void)
{
    backend_state_t audio_reset = {0};
    audio_reset.audio_decoder = 43;
    audio_reset.audio_au.address = audio_reset.audio_pcm;
    audio_reset.audio_au.length = 64;
    audio_reset.audio_pcm_item.address = audio_reset.audio_pcm;
    audio_reset.audio_pcm_item.length = 4096;
    assert(reset_native_audio_decoder(&audio_reset) == 0);
    assert(test_audio_deleted_handle == 43 && audio_reset.audio_decoder == 51);
    assert(test_audio_deletes == 1 && test_audio_creates == 1);
    test_audio_delete_result = -101;
    assert(reset_native_audio_decoder(&audio_reset) == -101);
    assert(audio_reset.audio_decoder == 51 && test_audio_creates == 1);
    test_audio_delete_result = 0;
    test_audio_create_result = -102;
    assert(reset_native_audio_decoder(&audio_reset) == -102);
    assert(audio_reset.audio_decoder == -102 && test_audio_creates == 2);
    assert(reset_native_audio_decoder(&audio_reset) == 0); // No native context to reset.
    assert(test_audio_deletes == 3);
    audio_reset.audio_decoder = 52;
    audio_reset.software_audio = (iptv_audio_decoder_t *)(uintptr_t)1;
    assert(reset_native_audio_decoder(&audio_reset) == 0); // Software fallback owns its reset.
    assert(audio_reset.audio_decoder == 52 && test_audio_deletes == 3);
    iptv_native_backend_t seeking = {0};
    assert(iptv_native_backend_init(&seeking) == 0);
    backend_state_t *seek = state_from(&seeking);
    // Restoring history before the replacement decoder opens has no old
    // submissions to cancel. No discontinuity callback will clear this flag.
    iptv_native_backend_request_reposition(&seeking);
    assert(!atomic_load(&seek->discard_input));
    assert(atomic_load(&seek->stream_generation) == 1);
    atomic_store(&seek->stream_generation, 0);
    seek->magic = BACKEND_MAGIC;
    seek->state = IPTV_NATIVE_STATE_OPEN;
    seek->config.enable_audio = 1;
    seek->video_queue = calloc(VIDEO_QUEUE_CAPACITY, sizeof(*seek->video_queue));
    assert(seek->video_queue);
    atomic_store(&seek->presented_pts_us, 123456);
    assert(iptv_native_backend_presented_pts(&seeking) == 123456);
    iptv_native_backend_set_paused(&seeking, 1);
    assert(iptv_native_backend_paused(&seeking));
    // A seek must release the demuxer even if pause filled the decoder queue.
    atomic_store(&seek->video_queue_write, VIDEO_QUEUE_CAPACITY);
    test_reposition_on_sleep = &seeking;
    const uint8_t frame[] = {0, 0, 1, 0x65};
    assert(queue_coded_frame(seek, frame, sizeof(frame), 123456, 1) == 0);
    assert(!test_reposition_on_sleep && !iptv_native_backend_paused(&seeking));
    assert(atomic_load(&seek->discard_input));
    assert(atomic_load(&seek->video_queue_write) == VIDEO_QUEUE_CAPACITY);
    assert(atomic_load(&seek->stream_generation) == 1);
    assert(iptv_native_backend_presented_pts(&seeking) == UINT64_MAX);
    assert(iptv_native_backend_discontinuity(&seeking) == 0);
    assert(!atomic_load(&seek->discard_input) && atomic_load(&seek->audio_sync_pending));
    assert(audio_sync_action(seek, 100000, 0) == 1); // Old picture cannot release rewound audio.
    assert(iptv_native_backend_presented_pts(&seeking) == UINT64_MAX);
    atomic_store(&seek->presented_generation, 2);
    assert(iptv_native_backend_presented_pts(&seeking) == 123456);
    assert(audio_sync_action(seek, 100000, 0) == 0);
    assert(audio_sync_action(seek, 200000, 0) == 1);
    assert(audio_sync_action(seek, 0, 0) == -1);
    assert(audio_sync_action(seek, 200000, 30000000) == 0); // Bad timestamps stay bounded.
    atomic_store(&seek->video_queue_write, 0);
    assert(queue_coded_frame(seek, frame, sizeof(frame), 100000, 1) == 0);
    assert(seek->video_queue[0].generation == 2);
    assert(seek->video_queue[0].pts_us == 100000);
    release_queued_video(seek);
    free(seek->video_queue);
    backend_state_t draining = {0};
    draining.video_thread = &draining;
    draining.config.poll_controls = test_drain_controls;
    draining.config.controls_context = &draining;
    assert(stop_video_worker(&draining) == 0);
    assert(test_control_polls == 3 && test_join_calls == 1);
    assert(atomic_load(&draining.stop_requested) && !draining.video_thread);
    audio_sink_t volume_sink = {0};
    volume_sink.handle = 7;
    volume_sink.applied_volume = -1;
    iptv_native_set_volume(100);
    assert(audio_sink_volume(&volume_sink) == 0);
    assert(test_volume_left == AUDIO_OUT_VOLUME_0DB && test_volume_right == AUDIO_OUT_VOLUME_0DB);
    iptv_native_set_volume(25);
    assert(audio_sink_volume(&volume_sink) == 0);
    assert(test_volume_left == AUDIO_OUT_VOLUME_0DB / 4 && test_volume_right == test_volume_left);
    iptv_native_set_volume(0);
    assert(audio_sink_volume(&volume_sink) == 0);
    assert(test_volume_left == 0 && test_volume_right == 0);
    assert(audio_sink_volume(&volume_sink) == 0 && test_volume_calls == 3);
    iptv_native_set_volume(200);
    assert(iptv_native_get_volume() == 100);
    assert(!media_span_ready(1, 0, 2000000, PLAYBACK_BUFFER_US));
    assert(!media_span_ready(60, 0, 1000000, PLAYBACK_BUFFER_US));
    assert(media_span_ready(121, 0, 2000000, PLAYBACK_BUFFER_US));
    assert(media_span_ready(51, 5000000, 7000000, PLAYBACK_BUFFER_US));
    assert(!media_span_ready(60, 5000000, 1000000, PLAYBACK_BUFFER_US));
    assert(!media_span_ready(60, 0, UINT64_C(100000000), PLAYBACK_BUFFER_US));
    backend_state_t gate = {0};
    gate.video_queue = calloc(VIDEO_QUEUE_CAPACITY, sizeof(*gate.video_queue));
    gate.audio_queue = calloc(AUDIO_QUEUE_CAPACITY, sizeof(*gate.audio_queue));
    assert(gate.video_queue && gate.audio_queue);
    atomic_store(&gate.audio_buffer_type, 0x0f);
    atomic_store(&gate.playback_gate_started_us, monotonic_us());
    assert(!playback_queues_ready(&gate));
    atomic_store(&gate.video_queue_write, 2);
    atomic_store(&gate.video_queue[1].pts_us, PLAYBACK_BUFFER_US);
    assert(!playback_queues_ready(&gate)); // Audio must also be buffered.
    assert(!video_queue_at_time_limit(&gate, 0, 2));
    atomic_store(&gate.video_queue[1].pts_us, DEMUX_BUFFER_US);
    assert(video_queue_at_time_limit(&gate, 0, 2)); // Limit startup before playback begins.
    assert(playback_queues_ready(&gate)); // Time pressure cannot deadlock underfilled audio.
    assert(!video_queue_at_time_limit(&gate, 0, 0));
    assert(!video_queue_at_time_limit(&gate, 0, 1));
    atomic_store(&gate.video_queue[1].pts_us, UINT64_MAX);
    assert(!video_queue_at_time_limit(&gate, 0, 2)); // Unknown clocks retain byte/frame limits.
    atomic_store(&gate.video_queue[1].pts_us, PLAYBACK_BUFFER_US);
    atomic_store(&gate.audio_queue_sample_rate, 48000);
    atomic_store(&gate.audio_queue_write, 94);
    assert(playback_queues_ready(&gate));
    atomic_store(&gate.audio_queue_write, 2);
    assert(!playback_queues_ready(&gate));
    atomic_store(&gate.audio_sync_pending, 1);
    assert(playback_queues_ready(&gate)); // A language switch must let video catch up to audio.
    atomic_store(&gate.audio_sync_pending, 0);
    assert(!playback_queues_ready(&gate)); // Ordinary A/V buffering resumes after alignment.
    atomic_store(&gate.video_queue_bytes, VIDEO_QUEUE_MAX_BYTES - INPUT_SLOT_BYTES);
    assert(playback_queues_ready(&gate)); // Release producer pressure.
    atomic_store(&gate.video_queue_bytes, 0);
    atomic_store(&gate.audio_buffer_type, 0);
    assert(playback_queues_ready(&gate)); // Silent channels need no audio.
    atomic_store(&gate.audio_buffer_type, 0x0f);
    atomic_store(&gate.playback_gate_started_us, monotonic_us() - PLAYBACK_START_TIMEOUT_US);
    assert(playback_queues_ready(&gate)); // Bounded fallback for bad timestamps.
    // A live seek must not reuse the old gate deadline or old audio pressure.
    atomic_store(&gate.playback_started, 1);
    atomic_store(&gate.stream_generation, 1);
    atomic_store(&gate.audio_sync_pending, 1);
    gate.video_queue[0].generation = gate.video_queue[1].generation = 1;
    atomic_store(&gate.video_queue[1].pts_us, 40000);
    atomic_store(&gate.audio_queue_write, AUDIO_QUEUE_CAPACITY - 1);
    restart_playback_buffer(&gate);
    assert(!atomic_load(&gate.playback_started));
    assert(!playback_queues_ready(&gate)); // Stale audio can drain while video waits.
    atomic_store(&gate.audio_generation, 1);
    assert(playback_queues_ready(&gate)); // Fresh queue pressure still releases the producer.
    atomic_store(&gate.audio_queue_write, 2);
    assert(!playback_queues_ready(&gate));
    atomic_store(&gate.video_queue[1].pts_us, PLAYBACK_BUFFER_US);
    assert(playback_queues_ready(&gate)); // Fresh video has its full startup cushion.
    atomic_store(&gate.stream_generation, 2);
    assert(!playback_queues_ready(&gate)); // Another seek invalidates the queued window.
    free(gate.audio_queue);
    free(gate.video_queue);
    iptv_native_backend_t programme_backend;
    assert(iptv_native_backend_init(&programme_backend) == 0);
    backend_state_t *programme = state_from(&programme_backend);
    programme->state = IPTV_NATIVE_STATE_OPEN;
    atomic_store(&programme->video_queue_write, 2);
    atomic_store(&programme->audio_queue_write, 3);
    atomic_store(&programme->audio_buffer_type, 0x0f);
    test_programme_drain_on_sleep = programme;
    assert(iptv_native_backend_programme_boundary(&programme_backend) == 0);
    assert(test_programme_drain_steps == 2); // Wait for both queues, not just video.
    assert(!atomic_load(&programme->programme_draining));
    atomic_store(&programme->paused, 1);
    atomic_store(&programme->video_queue_write, 4);
    test_reposition_on_sleep = &programme_backend;
    assert(iptv_native_backend_programme_boundary(&programme_backend) == 0);
    assert(atomic_load(&programme->video_queue_read) == 2); // Seek cancels a paused barrier.
    assert(!atomic_load(&programme->programme_draining));
    atomic_store(&programme->discard_input, 0);
    atomic_store(&programme->video_worker_result, -7);
    assert(iptv_native_backend_programme_boundary(&programme_backend) == -7);
    assert(!atomic_load(&programme->programme_draining));
    atomic_store(&programme->stop_requested, 1);
    assert(iptv_native_backend_programme_boundary(&programme_backend) == 0);
    atomic_store(&programme->stop_requested, 0);
    atomic_store(&programme->video_worker_result, 0);
    atomic_store(&programme->video_queue_read, 4);
    atomic_store(&programme->audio_queue_write, 6);
    atomic_store(&programme->audio_buffer_type, 0);
    assert(iptv_native_backend_programme_boundary(&programme_backend) == 0);
    pending_pts_t pending = {0};
    uint64_t pts_us;
    int displayable;
    uint32_t index;

    /* Decode order 0, 120, 40, 80 ms is the GOP cadence used by live HLS feeds. */
    assert(pending_pts_push(&pending, 0, 1));
    assert(pending_pts_push(&pending, 120000, 1));
    assert(pending_pts_push(&pending, 40000, 1));
    assert(pending_pts_push(&pending, 80000, 1));
    pending.values[0].fields = (iptv_field_info_t){1, 2, 20000, 0};
    pending.values[1].fields = (iptv_field_info_t){2, 2, 16683, 0};
    pending.values[2].fields = (iptv_field_info_t){1, 3, 20000, 0};
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == 0 &&
           displayable);
    assert(pending.taken_fields.first == 1 && pending.taken_fields.duration_us == 20000);
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == 40000 &&
           displayable);
    assert(pending.taken_fields.count == 3);
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == 80000 &&
           displayable);
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == 120000 &&
           displayable);
    assert(pending.taken_fields.first == 2 && pending.taken_fields.duration_us == 16683);
    assert(pending.count == 0);

    assert(pending_pts_push(&pending, UINT64_MAX, 1));
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == UINT64_MAX);
    assert(!pending_pts_take_smallest(&pending, &pts_us, &displayable));

    /* Interlaced: two fields in, one frame out. The second field's entry
     * leaves with the frame, whether it carried a timestamp or not. */
    assert(pending_pts_push(&pending, 0, 1));
    assert(pending_pts_push(&pending, UINT64_MAX, 1));
    assert(pending_pts_push(&pending, 40000, 1));
    assert(pending_pts_push(&pending, UINT64_MAX, 1));
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == 0);
    assert(pending_pts_drop_second_field(&pending) && pending.count == 2);
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == 40000);
    assert(pending_pts_drop_second_field(&pending) && pending.count == 0);
    assert(!pending_pts_drop_second_field(&pending));
    assert(pending_pts_push(&pending, 0, 1));
    assert(pending_pts_push(&pending, 20000, 1));
    assert(pending_pts_push(&pending, 40000, 1));
    assert(pending_pts_push(&pending, 60000, 1));
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == 0);
    assert(pending_pts_drop_second_field(&pending));
    assert(pending_pts_take_smallest(&pending, &pts_us, &displayable) && pts_us == 40000);
    assert(pending_pts_drop_second_field(&pending) && pending.count == 0);

    {
        /* A comb: lines of 0 and 200 alternate. Blended, every line but the
         * last is their mean; bytes past the width are left alone. */
        uint8_t picture[4 * 6 + 2 * 6];
        videodec2_output_t comb = {0};
        for (index = 0; index < sizeof(picture); ++index)
            picture[index] = (index / 6u) % 2u ? 200u : 0u;
        comb.buffer = picture;
        comb.pitch = 6;
        comb.width = 4;
        comb.height = 4;
        blend_fields(&comb);
        assert(picture[0] == 100 && picture[3] == 100 && picture[6] == 100 && picture[12] == 100);
        assert(picture[18] == 200);                    /* the last line has none under it */
        assert(picture[4] == 0 && picture[10] == 200); /* padding untouched */
        assert(picture[24] == 100 && picture[27] == 100 && picture[30] == 200);
    }

    for (index = 0; index < PENDING_PTS_CAPACITY; ++index)
        assert(pending_pts_push(&pending, index, 1));
    assert(!pending_pts_push(&pending, PENDING_PTS_CAPACITY, 1));
    for (index = 0; index < VIDEO_DRAIN_FLUSH_LIMIT; ++index)
        assert(pending_pts_take_first(&pending, &pts_us, &displayable) && pts_us == index);
    assert(pending.count == 0);
    return 0;
}
#endif
