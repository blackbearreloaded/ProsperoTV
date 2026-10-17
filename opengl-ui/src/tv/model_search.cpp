// ProsperoTV - One query across the active source and its on-demand catalogs.
// Copyright (C) 2026 BlackBearReloaded
// SPDX-License-Identifier: GPL-3.0-or-later
#include "tv/model.hpp"
#include "tv/i18n.hpp"

namespace ptv
{
void Model::begin_search()
{
    if (searching_)
        return;
    searching_ = true;
    // A search needs the lists themselves.
    search_movies_.configure_like(vod_);
    search_shows_.configure_like(vod_);
    search_movies_.select(VodKind::movies);
    search_shows_.select(VodKind::series);
}
void Model::end_search()
{
    searching_ = false;
    if (vod_overview_)
    {
        // Back to the counts alone.
        search_movies_.select_counts(VodKind::movies);
        search_shows_.select_counts(VodKind::series);
        return;
    }
    search_movies_.stop();
    search_shows_.stop();
}
void Model::set_vod_overview(bool active, bool force)
{
    if (active == vod_overview_ && !force)
        return;
    vod_overview_ = active;
    if (!active)
    {
        if (!searching_)
        {
            search_movies_.stop();
            search_shows_.stop();
        }
        return;
    }
    if (!searching_ || force)
    {
        // The browser may have refreshed a shared cache: read the summaries again.
        search_movies_.configure_like(vod_, true);
        search_shows_.configure_like(vod_, true);
        if (searching_)
        {
            search_movies_.select(VodKind::movies, {}, force);
            search_shows_.select(VodKind::series, {}, force);
        }
        else
        {
            search_movies_.select_counts(VodKind::movies, force);
            search_shows_.select_counts(VodKind::series, force);
        }
    }
}
std::optional<unsigned> Model::vod_count(VodKind kind) const
{
    if (!vod_.available())
        return 0;
    if (active_source_ == iptv::SourceKind::Custom && !has_catalog())
        return std::nullopt;
    const auto &library = kind == VodKind::movies ? search_movies_ : search_shows_;
    if (library.summary().valid)
    {
        // Hidden categories and the parental rules apply per category.
        unsigned count = 0;
        for (const auto &category : library.summary().categories)
        {
            if (category_hidden(category.name))
                continue;
            iptv::ChannelView view{};
            view.group_title = category.name;
            view.media_kind =
                kind == VodKind::movies ? iptv::MediaKind::movie : iptv::MediaKind::episode;
            if (content_allowed(view))
                count += category.titles - category.adult;
            view.adult = true;
            if (category.adult && content_allowed(view))
                count += category.adult;
        }
        return count;
    }
    if (!library.loaded())
        return std::nullopt;
    unsigned count = 0;
    for (const auto item : library.catalog())
        if (search_vod_allowed(item))
            ++count;
    return count;
}
bool Model::channel_visible(unsigned index) const
{
    return index < catalog_.size() && index < marks_.size() && !(marks_[index] & 4u) &&
           content_allowed(channel(index));
}
bool Model::search_vod_allowed(const iptv::ChannelView &item) const
{
    if (!content_allowed(item) || category_hidden(item.group_title))
        return false;
    for (const auto group : item.alternate_group_titles)
        if (category_hidden(group))
            return false;
    return true;
}
std::string Model::search_status() const
{
    const auto state = [](const VodLibrary &library)
    {
        return library.busy() || library.requested() ? std::string(tr("Loading"))
               : !library.available() ? std::string(tr("Unavailable for this source"))
                                      : library.status();
    };
    return std::string(tr("Movies")) + ": " + state(search_movies_) + " / " + tr("TV shows") +
           ": " + state(search_shows_) + " / " + tr("Guide") + ": " +
           (guide_refreshing() ? tr("Loading")
            : guide_.count()   ? tr("Saved programmes")
                               : tr("No guide available"));
}
bool Model::activate_search_vod(const SearchResult &result)
{
    if (!searching_ || result.source_id != source_id(active_source_) || pin_prompt() ||
        (result.kind != SearchResult::Kind::movie && result.kind != SearchResult::Kind::show))
        return false;
    const auto &library = result.kind == SearchResult::Kind::movie ? search_movies_ : search_shows_;
    const auto at = library.catalog().Find(result.id);
    if (at == iptv::Catalog::npos)
        return false;
    const auto item = library.catalog()[at];
    if (!search_vod_allowed(item))
        return false;
    if (result.kind == SearchResult::Kind::movie)
        return queue_vod(item, {});
    view.vod_series = item.tvg_id;
    view.vod_series_name = item.name;
    view.vod_series_cover = item.tvg_logo;
    view.vod_series_group = item.group_title;
    view.vod_series_adult = item.adult;
    view.vod_kind = static_cast<int>(VodKind::episodes);
    view.vod_category.clear();
    view.vod_query.clear();
    view.vod_all = false;
    view.vod_focus = 0;
    vod_.select(VodKind::episodes, view.vod_series);
    return true;
}
} // namespace ptv
