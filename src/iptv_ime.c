/* ProsperoTV - native PS5 IPTV client derived from ps5-native-app-boilerplate.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later */

#include "iptv_ime.h"

#include "iptv_input.h"

#include <SDL2/SDL.h>
#include <stddef.h>
#include <stdint.h>

#define SCE_SYSMODULE_IME_DIALOG UINT16_C(0x0096)
#define SCE_COMMON_DIALOG_ALREADY_INITIALIZED UINT32_C(0x80B80002)
#define SCE_IME_TYPE_DEFAULT INT32_C(0)
#define SCE_IME_TYPE_BASIC_LATIN INT32_C(1)
#define SCE_IME_ENTER_LABEL_DEFAULT INT32_C(0)
#define SCE_IME_ENTER_LABEL_SEARCH INT32_C(2)
#define SCE_IME_OPTION_PASSWORD UINT32_C(0x00000004)
#define IME_TITLE_CHARACTERS 47U
#define IME_PLACEHOLDER_CHARACTERS 95U

typedef struct
{
    int32_t user_id;
    int32_t type;
    uint64_t supported_languages;
    int32_t enter_label;
    int32_t input_method;
    void *filter;
    uint32_t option;
    uint32_t max_text_length;
    uint16_t *input_text_buffer;
    float pos_x;
    float pos_y;
    int32_t horizontal_alignment;
    int32_t vertical_alignment;
    const uint16_t *placeholder;
    const uint16_t *title;
    int8_t reserved[16];
} sce_ime_dialog_param_t;

typedef struct
{
    int32_t outcome;
    int8_t reserved[12];
} sce_ime_dialog_result_t;

_Static_assert(sizeof(sce_ime_dialog_param_t) == 96U, "unexpected PS5 IME parameter layout");
_Static_assert(sizeof(sce_ime_dialog_result_t) == 16U, "unexpected PS5 IME result layout");

extern int sceCommonDialogInitialize(void);
extern int sceImeDialogAbort(void);
extern int sceImeDialogGetResult(sce_ime_dialog_result_t *result);
extern int sceImeDialogGetStatus(void);
extern int sceImeDialogInit(const sce_ime_dialog_param_t *param, const void *extended);
extern int sceImeDialogTerm(void);
extern int sceSysmoduleLoadModule(uint16_t module_id);
extern int sceSysmoduleUnloadModule(uint16_t module_id);
extern int sceUserServiceGetForegroundUser(int32_t *user_id);

static bool module_loaded;
static bool requested;
static bool active;
static uint32_t started_at;
static char initial_text[IPTV_IME_BUFFER_BYTES];
static char requested_title[IME_TITLE_CHARACTERS * 4U + 1U];
static char requested_placeholder[IME_PLACEHOLDER_CHARACTERS * 4U + 1U];
static uint16_t text_buffer[IPTV_IME_BUFFER_CHARACTERS + 1U];
static uint16_t placeholder[IME_PLACEHOLDER_CHARACTERS + 1U];
static uint16_t title[IME_TITLE_CHARACTERS + 1U];
static unsigned requested_characters;
static int32_t requested_type;
static int32_t requested_enter_label;
static uint32_t requested_option;
static iptv_ime_result_fn result_callback;
static void *result_user_data;

static void clear_sensitive(void *memory, size_t size)
{
    volatile unsigned char *bytes = memory;
    while (size-- != 0)
        *bytes++ = 0;
}

static void request_with_options(const char *value, const char *dialog_title,
                                 const char *dialog_placeholder, unsigned max_text_characters,
                                 int32_t type, int32_t enter_label, uint32_t option,
                                 iptv_ime_result_fn callback, void *user_data);

static void utf8_to_utf16(const char *source, uint16_t *output, size_t capacity)
{
    size_t written = 0;
    const unsigned char *text = (const unsigned char *)(source != NULL ? source : "");
    while (*text != 0 && written + 1U < capacity)
    {
        uint32_t codepoint = *text;
        unsigned bytes = 1;
        bool valid = true;
        if ((text[0] & 0xe0U) == 0xc0U)
        {
            codepoint = text[0] & 0x1fU;
            bytes = 2;
        }
        else if ((text[0] & 0xf0U) == 0xe0U)
        {
            codepoint = text[0] & 0x0fU;
            bytes = 3;
        }
        else if ((text[0] & 0xf8U) == 0xf0U)
        {
            codepoint = text[0] & 0x07U;
            bytes = 4;
        }
        else if ((text[0] & 0x80U) != 0)
        {
            valid = false;
        }
        if (bytes > 1)
        {
            for (unsigned i = 1; i < bytes; ++i)
            {
                if (text[i] == 0 || (text[i] & 0xc0U) != 0x80U)
                {
                    valid = false;
                    break;
                }
                codepoint = (codepoint << 6) | (text[i] & 0x3fU);
            }
            const uint32_t minimum = bytes == 2 ? 0x80U : bytes == 3 ? 0x800U : 0x10000U;
            if (codepoint < minimum || codepoint > 0x10ffffU ||
                (codepoint >= 0xd800U && codepoint <= 0xdfffU))
                valid = false;
        }
        if (!valid)
        {
            codepoint = 0xfffdU;
            bytes = 1;
        }
        text += bytes;
        if (codepoint >= 0x10000U)
        {
            if (written + 2U >= capacity)
                break;
            codepoint -= 0x10000U;
            output[written++] = (uint16_t)(0xd800U | (codepoint >> 10));
            output[written++] = (uint16_t)(0xdc00U | (codepoint & 0x3ffU));
        }
        else
        {
            output[written++] = (uint16_t)codepoint;
        }
    }
    output[written] = 0;
}

static void utf16_to_utf8(const uint16_t *source, char *output, size_t capacity)
{
    size_t written = 0;
    for (size_t i = 0; source[i] != 0; ++i)
    {
        uint32_t codepoint = source[i];
        if (codepoint >= 0xd800U && codepoint <= 0xdbffU && source[i + 1U] >= 0xdc00U &&
            source[i + 1U] <= 0xdfffU)
        {
            codepoint = 0x10000U + ((codepoint - 0xd800U) << 10) + (source[++i] - 0xdc00U);
        }
        else if (codepoint >= 0xd800U && codepoint <= 0xdfffU)
        {
            codepoint = 0xfffdU;
        }
        const unsigned bytes = codepoint < 0x80U      ? 1U
                               : codepoint < 0x800U   ? 2U
                               : codepoint < 0x10000U ? 3U
                                                      : 4U;
        if (written + bytes >= capacity)
            break;
        if (bytes == 1U)
        {
            output[written++] = (char)codepoint;
        }
        else
        {
            for (unsigned byte = bytes - 1U; byte != 0; --byte)
            {
                output[written + byte] =
                    (char)(0x80U | ((codepoint >> (6U * (bytes - 1U - byte))) & 0x3fU));
            }
            output[written] = (char)((0xf0U << (4U - bytes)) | (codepoint >> (6U * (bytes - 1U))));
            written += bytes;
        }
    }
    output[written] = '\0';
}

bool iptv_ime_init(void)
{
    if (module_loaded)
        return true;
    const int common_dialog = sceCommonDialogInitialize();
    if (common_dialog < 0 && (uint32_t)common_dialog != SCE_COMMON_DIALOG_ALREADY_INITIALIZED)
        return false;
    if (sceSysmoduleLoadModule(SCE_SYSMODULE_IME_DIALOG) < 0)
        return false;
    module_loaded = true;
    return true;
}

void iptv_ime_request(const char *value, iptv_ime_result_fn callback, void *user_data)
{
    request_with_options(value, "Search ProsperoTV", "Name, country, language, or category",
                         IPTV_IME_MAX_TEXT_CHARACTERS, SCE_IME_TYPE_DEFAULT,
                         SCE_IME_ENTER_LABEL_SEARCH, 0, callback, user_data);
}

static void request_with_options(const char *value, const char *dialog_title,
                                 const char *dialog_placeholder, unsigned max_text_characters,
                                 int32_t type, int32_t enter_label, uint32_t option,
                                 iptv_ime_result_fn callback, void *user_data)
{
    if (active || requested || !module_loaded)
        return;
    if (max_text_characters == 0 || max_text_characters > IPTV_IME_BUFFER_CHARACTERS)
        return;
    SDL_strlcpy(initial_text, value != NULL ? value : "", sizeof(initial_text));
    SDL_strlcpy(requested_title, dialog_title != NULL ? dialog_title : "", sizeof(requested_title));
    SDL_strlcpy(requested_placeholder, dialog_placeholder != NULL ? dialog_placeholder : "",
                sizeof(requested_placeholder));
    requested_characters = max_text_characters;
    requested_type = type;
    requested_enter_label = enter_label;
    requested_option = option;
    result_callback = callback;
    result_user_data = user_data;
    requested = true;
}

void iptv_ime_request_prompt(const char *value, const char *dialog_title,
                             const char *dialog_placeholder, unsigned max_text_characters,
                             iptv_ime_result_fn callback, void *user_data)
{
    request_with_options(value, dialog_title, dialog_placeholder, max_text_characters,
                         SCE_IME_TYPE_DEFAULT, SCE_IME_ENTER_LABEL_DEFAULT, 0, callback, user_data);
}

void iptv_ime_request_password(const char *dialog_title, const char *dialog_placeholder,
                               unsigned max_text_characters, iptv_ime_result_fn callback,
                               void *user_data)
{
    request_with_options("", dialog_title, dialog_placeholder, max_text_characters,
                         SCE_IME_TYPE_BASIC_LATIN, SCE_IME_ENTER_LABEL_DEFAULT,
                         SCE_IME_OPTION_PASSWORD, callback, user_data);
}

static void start_requested(void)
{
    int32_t user_id = -1;
    if (sceUserServiceGetForegroundUser(&user_id) < 0)
    {
        requested = false;
        clear_sensitive(initial_text, sizeof(initial_text));
        clear_sensitive(text_buffer, sizeof(text_buffer));
        requested_enter_label = SCE_IME_ENTER_LABEL_DEFAULT;
        return;
    }
    utf8_to_utf16(initial_text, text_buffer, sizeof(text_buffer) / sizeof(text_buffer[0]));
    utf8_to_utf16(requested_placeholder, placeholder, sizeof(placeholder) / sizeof(placeholder[0]));
    utf8_to_utf16(requested_title, title, sizeof(title) / sizeof(title[0]));
    const sce_ime_dialog_param_t param = {
        .user_id = user_id,
        .type = requested_type,
        .enter_label = requested_enter_label,
        .option = requested_option,
        .max_text_length = requested_characters,
        .input_text_buffer = text_buffer,
        .horizontal_alignment = 1,
        .vertical_alignment = 1,
        .placeholder = placeholder,
        .title = title,
    };
    active = sceImeDialogInit(&param, NULL) == 0;
    started_at = SDL_GetTicks();
    requested = false;
}

void iptv_ime_poll(void)
{
    if (requested && !iptv_input_pressed(IPTV_INPUT_CROSS))
        start_requested();
    if (!active)
        return;

    const int status = sceImeDialogGetStatus();
    if (status == 1 || (status == 0 && SDL_GetTicks() - started_at < 1000U))
        return;
    if (status == 2)
    {
        sce_ime_dialog_result_t result = {0};
        if (sceImeDialogGetResult(&result) >= 0 && result.outcome == 0 && result_callback != NULL)
        {
            char text[IPTV_IME_BUFFER_BYTES];
            utf16_to_utf8(text_buffer, text, sizeof(text));
            result_callback(text, result_user_data);
            clear_sensitive(text, sizeof(text));
        }
        sceImeDialogTerm();
    }
    clear_sensitive(initial_text, sizeof(initial_text));
    clear_sensitive(text_buffer, sizeof(text_buffer));
    requested_type = SCE_IME_TYPE_DEFAULT;
    requested_enter_label = SCE_IME_ENTER_LABEL_DEFAULT;
    requested_option = 0;
    active = false;
}

void iptv_ime_cancel(void)
{
    requested = false;
    if (active)
        sceImeDialogAbort();
    clear_sensitive(initial_text, sizeof(initial_text));
    clear_sensitive(text_buffer, sizeof(text_buffer));
    requested_type = SCE_IME_TYPE_DEFAULT;
    requested_enter_label = SCE_IME_ENTER_LABEL_DEFAULT;
    requested_option = 0;
}

void iptv_ime_shutdown(void)
{
    iptv_ime_cancel();
    if (module_loaded)
    {
        sceSysmoduleUnloadModule(SCE_SYSMODULE_IME_DIALOG);
        module_loaded = false;
    }
    active = false;
    result_callback = NULL;
    result_user_data = NULL;
    requested_characters = 0;
}
