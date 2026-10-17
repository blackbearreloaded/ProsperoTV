// ProsperoTV - Home lists and saved on-demand playback.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/model.hpp"
#include "tv/i18n.hpp"
#include "tv/platform.hpp"
#include <algorithm>
#include <chrono>

namespace ptv
{
namespace
{
std::uint64_t home_clock()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace
std::array<std::vector<Model::HomeItem>, 3> Model::home_items() const
{
    std::array<std::vector<HomeItem>, 3> rows;
    const auto live = [&](unsigned at)
    {
        const auto item = channel(at);
        const auto *programme =
            guide_.now(item.id, static_cast<std::int64_t>(platform::unix_time()));
        return HomeItem{std::string(item.id), std::string(item.name),
                        programme ? programme->title : std::string(tr("No programme information")),
                        std::string(item.tvg_logo)};
    };
    for (const auto &item : library_.unfinished_vod(selected_source_id_, source_id(active_source_)))
        if (home_vod_allowed(item))
            rows[0].push_back({item.channel.id, item.channel.name, item.channel.group_title,
                               item.channel.tvg_logo, true, item.position_us, item.duration_us});
    const auto last = library_.last_channel();
    const auto at = catalog_.Find(last.second);
    if (last.first == selected_source_id_ && at != iptv::Catalog::npos && channel_visible(at))
        rows[0].push_back(live(at));
    std::unordered_set<unsigned> saved_favorites;
    for (const auto &id : favorite_order_)
    {
        const auto index = catalog_.Find(id);
        if (index != iptv::Catalog::npos && channel_visible(index) && is_favorite(channel(index)))
        {
            rows[1].push_back(live(index));
            saved_favorites.insert(index);
            if (rows[1].size() == 50)
                break;
        }
    }
    std::vector<unsigned> usual;
    for (unsigned index : index_.order)
    {
        if (!channel_visible(index))
            continue;
        if (is_favorite(channel(index)) && !saved_favorites.contains(index) && rows[1].size() < 50)
            rows[1].push_back(live(index));
        if (index < channel_history_.size() && channel_history_[index].watches > 0)
            usual.push_back(index);
    }
    std::partial_sort(usual.begin(), usual.begin() + std::min<std::size_t>(20, usual.size()),
                      usual.end(),
                      [&](unsigned a, unsigned b)
                      {
                          return channel_history_[a].watches == channel_history_[b].watches
                                     ? a < b
                                     : channel_history_[a].watches > channel_history_[b].watches;
                      });
    for (unsigned index : usual)
    {
        rows[2].push_back(live(index));
        if (rows[2].size() == 20)
            break;
    }
    for (const auto &column : rows)
        for (const auto &item : column)
        {
            note_scripts(item.title, &home_east_asian_, &home_korean_);
            note_scripts(item.detail, &home_east_asian_, &home_korean_);
        }
    return rows;
}
bool Model::home_vod_allowed(const ResumeItem &item) const
{
    if (hidden_channels_.contains(item.channel.id) || !search_vod_allowed(item.channel))
        return false;
    // A newer loaded provider list can strengthen the saved item's restrictions.
    for (const auto *catalog : {&catalog_, &vod_.catalog(), &search_movies_.catalog(),
                                &search_shows_.catalog(), &home_series_.catalog()})
        if (const auto at = catalog->Find(item.channel.id);
            at != iptv::Catalog::npos && !search_vod_allowed((*catalog)[at]))
            return false;
    for (const auto *shows : {&vod_, &search_shows_})
        if (const auto at = shows->find_series(item.series, item.channel.source_id);
            at != iptv::Catalog::npos)
        {
            const auto parent = shows->catalog()[at];
            if (hidden_channels_.contains(std::string(parent.id)) || !search_vod_allowed(parent))
                return false;
        }
    return true;
}
bool Model::queue_home_vod(ResumeItem item)
{
    const auto inherit_policy = [&](const iptv::ChannelView &current)
    {
        item.channel.adult = item.channel.adult || current.adult;
        const auto add_group = [&](std::string_view group)
        {
            auto &groups = item.channel.alternate_group_titles;
            if (!group.empty() && group != item.channel.group_title &&
                std::find(groups.begin(), groups.end(), group) == groups.end())
                groups.emplace_back(group);
        };
        add_group(current.group_title);
        for (const auto group : current.alternate_group_titles)
            add_group(group);
    };
    bool stream_updated = false;
    const iptv::Catalog *catalogs[] = {&home_series_.catalog(), &vod_.catalog(),
                                       &search_movies_.catalog(), &search_shows_.catalog(),
                                       &catalog_};
    for (const auto *catalog : catalogs)
        if (const auto at = catalog->Find(item.channel.id); at != iptv::Catalog::npos)
        {
            const auto current = (*catalog)[at];
            if (current.source_id != item.channel.source_id)
                continue;
            inherit_policy(current);
            if (!stream_updated)
            {
                const auto copy = current.Copy();
                item.channel.url = copy.url;
                item.channel.alternate_urls = copy.alternate_urls;
                item.channel.http_user_agent = copy.http_user_agent;
                item.channel.http_referrer = copy.http_referrer;
                stream_updated = true;
            }
        }
    for (const auto *shows : {&vod_, &search_shows_})
        if (const auto at = shows->find_series(item.series, item.channel.source_id);
            at != iptv::Catalog::npos)
            inherit_policy(shows->catalog()[at]);
    if (item.channel.alternate_group_titles.size() > 4 || !home_vod_allowed(item))
        return false;
    if (!item.series.empty())
        home_playback_ = item;
    return queue_vod(item.channel, item.series, false);
}
bool Model::play_home(std::string_view id, bool on_demand)
{
    if (!playback_allowed() || pin_prompt())
        return false;
    clear_home_playback();
    if (!on_demand)
        return play_channel(id);
    for (const auto &item : library_.unfinished_vod(selected_source_id_, source_id(active_source_)))
    {
        if (item.channel.id != id || !home_vod_allowed(item))
            continue;
        if (!item.series.empty())
        {
            home_series_.configure_like(vod_);
            home_series_.select(VodKind::episodes, item.series);
            home_playback_ = item;
            home_source_ = selected_source_id_;
            home_fingerprint_ = source_id(active_source_);
            if (!home_series_.loaded() && (home_series_.requested() || home_series_.loading()))
            {
                // A read of the saved episodes is waited for in silence; a download is announced.
                home_waiting_ = true;
                home_deadline_ = home_clock() + 10000;
                home_notified_ = false;
                notify_home_download();
                return true;
            }
            home_series_.stop();
            if (!home_vod_allowed(item))
            {
                clear_home_playback();
                return false;
            }
        }
        // The snapshot already includes its show's policy; leave the VOD browser intact.
        return queue_home_vod(item);
    }
    return false;
}
void Model::notify_home_download()
{
    if (home_notified_ || !home_waiting_ || !home_series_.requested())
        return;
    home_notified_ = true;
    notify(Level::busy, tr("Loading episodes before playback"), tr("Press Circle to cancel."));
}
void Model::clear_home_playback()
{
    home_series_.configure({}, {}, RefreshSchedule::manual);
    home_waiting_ = false;
    home_notified_ = false;
    home_playback_ = {};
    home_source_ = 0;
    home_fingerprint_ = 0;
}
bool Model::cancel_home_play()
{
    if (!home_waiting_)
        return false;
    clear_home_playback();
    return true;
}
void Model::poll_home_playback()
{
    if (!home_waiting_)
        return;
    if (home_source_ != selected_source_id_ || home_fingerprint_ != source_id(active_source_) ||
        !home_vod_allowed(home_playback_) || pin_prompt() || play_requested_)
    {
        clear_home_playback();
        return;
    }
    if (home_clock() >= home_deadline_)
        home_series_.stop();
    else
        home_series_.poll(!refreshing() && !guide_thread_ && !vod_.busy() && !search_busy());
    notify_home_download();
    if (home_series_.busy() || home_series_.requested())
        return;
    home_waiting_ = false;
    if (!home_vod_allowed(home_playback_))
    {
        clear_home_playback();
        notify(Level::warning, tr("Channel unavailable or locked"));
        return;
    }
    if (!home_series_.loaded())
        notify(
            Level::warning, tr("Next episode unavailable"),
            tr("Starting your saved episode. Resume from Home again to retry the episode list."));
    queue_home_vod(home_playback_);
}
bool Model::next_home_episode(const PlayRequest &request)
{
    if (home_source_ != selected_source_id_ || home_fingerprint_ != source_id(active_source_) ||
        request.source_id != home_fingerprint_ || request.series_id != home_playback_.series ||
        request.channel_id != home_playback_.channel.id || pin_prompt() ||
        !home_vod_allowed(home_playback_))
        return false;
    const auto &catalog = home_series_.catalog();
    const auto current = catalog.Find(request.channel_id);
    if (!home_series_.loaded() || current == iptv::Catalog::npos)
        return false;
    unsigned next = static_cast<unsigned>(catalog.size());
    for (unsigned i = 0; i < catalog.size(); ++i)
        if (catalog[i].source_line > catalog[current].source_line &&
            (next == catalog.size() || catalog[i].source_line < catalog[next].source_line))
            next = i;
    if (next == catalog.size())
        return false;
    ResumeItem item = home_playback_;
    item.channel = catalog[next].Copy();
    // Keep the show's inherited policy, and any stronger episode policy.
    item.channel.adult = item.channel.adult || home_playback_.channel.adult;
    const auto add_group = [&](const std::string &group)
    {
        auto &groups = item.channel.alternate_group_titles;
        if (!group.empty() && group != item.channel.group_title &&
            std::find(groups.begin(), groups.end(), group) == groups.end())
            groups.push_back(group);
    };
    add_group(home_playback_.channel.group_title);
    for (const auto &group : home_playback_.channel.alternate_group_titles)
        add_group(group);
    if (item.channel.alternate_group_titles.size() > 4)
        return false;
    if (!home_vod_allowed(item))
        return false;
    home_playback_ = std::move(item);
    return queue_home_vod(home_playback_);
}
} // namespace ptv
