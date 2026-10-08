// ProsperoTV - What every screen shares: the logic, the theme, settings, toasts.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "tv/kit.hpp"
#include "tv/model.hpp"
#include "tv/settings.hpp"
#include "tv/theme.hpp"
#include "tv/images.hpp"
#include "tv/preview.hpp"
#include "ui/components/toast.hpp"

namespace ptv
{

struct Shared
{
    Shared(Model &the_model, const ui::Fonts &the_fonts, const Settings &the_settings)
        : model(the_model), fonts(the_fonts), theme(dusk()), settings(the_settings)
    {
    }

    Model &model;
    const ui::Fonts &fonts;
    ui::Theme theme;
    Settings settings;
    ui::ToastStack toasts;
    ImageCache images;
    LivePreview preview;
    float clock = 0.0f; // free-running seconds, for idle motion
};

} // namespace ptv
