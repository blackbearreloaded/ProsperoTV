// ProsperoTV - Parental controls shared by browsing and every playback path.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/i18n.hpp"
#include "tv/model.hpp"
#include "tv/category_path.hpp"
#include "tv/platform.hpp"
#include "iptv_ime.h"
#include <openssl/crypto.h>

namespace ptv
{
bool Model::require_parent()
{
    if (parental_.unlocked())
        return true;
    notify(Level::warning, tr("Parent PIN required"),
           tr("Unlock parental controls in Settings first."));
    return false;
}
int Model::category_rule(std::string_view category, bool inherited) const
{
    int result = 0;
    for (category = category_trim(category); !category.empty();
         category = category_parent(category))
    {
        const auto found = category_rules_.find(std::string(category));
        if (found != category_rules_.end())
        {
            if (found->second == 1)
                return 1; // A child's approval cannot override an adult parent.
            if (found->second == 2)
                result = 2;
        }
        if (!inherited)
            break;
    }
    return result;
}
bool Model::content_allowed(const iptv::ChannelView &channel) const
{
    if (!parental_.valid() || (parental_.enabled() && !library_.ready()))
        return false;
    if (!parental_.enabled())
        return true;
    bool adult = channel.adult;
    bool kids = false;
    const auto category = [&](std::string_view name)
    {
        const int rule = category_rule(name);
        adult = adult || adult_category(name) || rule == 1;
        kids = kids || kids_category(name) || rule == 2;
    };
    category(channel.group_title);
    for (auto other : channel.alternate_group_titles)
        category(other);
    return parental_.kids_only() ? kids && !adult : !adult || parental_.unlocked();
}
bool Model::vod_allowed(unsigned index) const
{
    if (index >= vod_.catalog().size())
        return false;
    const auto item = vod_.catalog()[index];
    if (vod_.kind() != VodKind::episodes)
        return content_allowed(item);
    // Episodes inherit the selected show's category even after loading a cached episode list.
    auto episode = item;
    episode.group_title = view.vod_series_group;
    episode.adult = episode.adult || view.vod_series_adult;
    return content_allowed(episode);
}
bool Model::set_category_rule(std::string_view category, int rule)
{
    if (!parental_.enabled() || !require_parent() ||
        !library_.set_category_rule(source_id(active_source_), category, rule))
        return false;
    category_rules_ = library_.category_rules(source_id(active_source_));
    parental_changed();
    return true;
}
void Model::parental_changed()
{
    play_requested_ = false;
    play_request_ = {};
    mark_visibility();
    recount_groups();
    rebuild_visible();
}
void Model::clear_pin()
{
    OPENSSL_cleanse(first_pin_.data(), first_pin_.size());
    first_pin_.clear();
    pin_step_ = PinStep::none;
    pin_pending_ = false;
}
void Model::parental_action(ParentalAction action)
{
    if (iptv_ime_busy() || !keyboard_ready_)
        return;
    clear_pin();
    if (!parental_.valid())
    {
        notify(
            Level::error, tr("Parental settings could not be read"),
            tr("Restore the profile's parental settings from a trusted backup outside the app."));
        return;
    }
    if (action == ParentalAction::unlock)
    {
        if (!parental_.enabled())
            return;
        if (platform::unix_time() < parental_.wait_until())
        {
            notify(Level::warning, tr("Too many PIN attempts"),
                   tr("Wait 30 seconds before trying again."));
            return;
        }
        pin_step_ = PinStep::unlock;
    }
    else if (action == ParentalAction::lock)
    {
        parental_.lock();
        parental_changed();
        notify(Level::ready, tr("Parental controls locked"));
        return;
    }
    else
    {
        if (!require_parent())
            return;
        if (action == ParentalAction::set_pin)
            pin_step_ = PinStep::create;
        else
        {
            const bool ok = action == ParentalAction::kids
                                ? parental_.set_kids(!parental_.kids_only())
                                : parental_.remove();
            if (!ok)
                notify(Level::error, tr("Parental settings could not be saved"),
                       tr("Set a PIN before enabling kids mode."));
            else
                parental_changed();
            return;
        }
    }
    pin_pending_ = true;
}
void Model::poll_pin()
{
    if (pin_pending_ && !iptv_ime_busy())
    {
        pin_pending_ = false;
        iptv_ime_request_password(pin_step_ == PinStep::create    ? tr("New parent PIN")
                                  : pin_step_ == PinStep::confirm ? tr("Confirm parent PIN")
                                                                  : tr("Parent PIN"),
                                  tr("4 to 8 digits"), 8, &Model::on_pin, this);
    }
    else if (pin_step_ != PinStep::none && !iptv_ime_busy())
        clear_pin();
}
void Model::on_pin(const char *text, void *self)
{
    auto &model = *static_cast<Model *>(self);
    if (!text)
    {
        model.clear_pin();
        return;
    }
    if (model.pin_step_ == PinStep::create)
    {
        if (!Parental::valid_pin(text))
        {
            model.notify(Level::warning, tr("Use a PIN of 4 to 8 digits"));
            model.clear_pin();
            return;
        }
        model.first_pin_ = text;
        model.pin_step_ = PinStep::confirm;
        model.pin_pending_ = true;
        return;
    }
    const bool unlocking = model.pin_step_ == PinStep::unlock;
    const bool ok = unlocking ? model.parental_.unlock(text, platform::unix_time())
                              : model.pin_step_ == PinStep::confirm && model.first_pin_ == text &&
                                    model.parental_.set_pin(text);
    model.clear_pin();
    model.parental_changed();
    model.notify(ok ? Level::ready : Level::warning,
                 ok ? (unlocking ? tr("Parental controls unlocked for this session")
                                 : tr("Parent PIN saved; controls locked"))
                    : tr("PIN not accepted"),
                 ok ? ""
                    : tr("Check the PIN and available storage. Five failed attempts require a "
                         "30-second wait."));
}
} // namespace ptv
