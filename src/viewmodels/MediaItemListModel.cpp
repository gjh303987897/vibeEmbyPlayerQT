#include "viewmodels/MediaItemListModel.h"

#include <QSet>

#include <algorithm>
#include <cmath>
#include <iterator>

MediaItemListModel::MediaItemListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int MediaItemListModel::count() const
{
    return static_cast<int>(m_items.size());
}

int MediaItemListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return count();
}

QVariant MediaItemListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& item = m_items[static_cast<size_t>(index.row())];
    switch (role) {
    case IdRole:
        return item.id;
    case NameRole:
        return item.name;
    case ItemTypeRole:
        return item.itemType;
    case ProductionYearRole:
        return item.productionYear;
    case SeriesIdRole:
        return item.seriesId;
    case SeriesNameRole:
        return item.seriesName;
    case SeriesImageUrlRole:
        return item.seriesImageUrl;
    case ChildCountRole:
        return item.childCount;
    case ContinueImageUrlRole:
        return item.seriesImageUrl.isEmpty() ? item.imageUrl : item.seriesImageUrl;
    case OverviewRole:
        return item.overview;
    case ImageUrlRole:
        return item.imageUrl.isEmpty() ? item.seriesImageUrl : item.imageUrl;
    case BackdropImageUrlRole:
        return item.backdropImageUrl;
    case CommunityRatingRole:
        return item.communityRating;
    case OfficialRatingRole:
        return item.officialRating;
    case RunTimeRole:
        return item.runTime;
    case GenresRole:
        return item.genres;
    case PeopleRole:
        return item.people;
    case SeasonNameRole:
        return item.seasonName;
    case IndexNumberRole:
        return item.indexNumber;
    case ParentIndexNumberRole:
        return item.parentIndexNumber;
    case PlayedPercentageRole:
        return item.playedPercentage;
    case PlayedRole:
        return item.played;
    case PlaybackPositionTicksRole:
        return item.playbackPositionTicks;
    default:
        return {};
    }
}

QHash<int, QByteArray> MediaItemListModel::roleNames() const
{
    return {
        { IdRole, "itemId" },
        { NameRole, "name" },
        { ItemTypeRole, "itemType" },
        { ProductionYearRole, "productionYear" },
        { SeriesIdRole, "seriesId" },
        { SeriesNameRole, "seriesName" },
        { SeriesImageUrlRole, "seriesImageUrl" },
        { ChildCountRole, "childCount" },
        { ContinueImageUrlRole, "continueImageUrl" },
        { OverviewRole, "overview" },
        { ImageUrlRole, "imageUrl" },
        { BackdropImageUrlRole, "backdropImageUrl" },
        { CommunityRatingRole, "communityRating" },
        { OfficialRatingRole, "officialRating" },
        { RunTimeRole, "runTime" },
        { GenresRole, "genres" },
        { PeopleRole, "people" },
        { SeasonNameRole, "seasonName" },
        { IndexNumberRole, "indexNumber" },
        { ParentIndexNumberRole, "parentIndexNumber" },
        { PlayedPercentageRole, "playedPercentage" },
        { PlayedRole, "played" },
        { PlaybackPositionTicksRole, "playbackPositionTicks" },
    };
}

void MediaItemListModel::setItems(std::vector<MediaItem> items)
{
    beginResetModel();
    m_items = std::move(items);
    endResetModel();
    emit countChanged();
}

int MediaItemListModel::appendItems(std::vector<MediaItem> items)
{
    if (items.empty()) {
        return 0;
    }

    QSet<QString> existingIds;
    existingIds.reserve(static_cast<qsizetype>(m_items.size() + items.size()));
    for (const auto& item : m_items) {
        if (!item.id.isEmpty()) {
            existingIds.insert(item.id);
        }
    }

    std::vector<MediaItem> uniqueItems;
    uniqueItems.reserve(items.size());
    for (auto& item : items) {
        if (!item.id.isEmpty() && existingIds.contains(item.id)) {
            continue;
        }
        if (!item.id.isEmpty()) {
            existingIds.insert(item.id);
        }
        uniqueItems.push_back(std::move(item));
    }

    if (uniqueItems.empty()) {
        return 0;
    }

    const auto first = rowCount();
    const auto appendedCount = static_cast<int>(uniqueItems.size());
    const auto last = first + appendedCount - 1;
    beginInsertRows({}, first, last);
    m_items.insert(m_items.end(), std::make_move_iterator(uniqueItems.begin()), std::make_move_iterator(uniqueItems.end()));
    endInsertRows();
    emit countChanged();
    return appendedCount;
}

bool MediaItemListModel::updatePlaybackProgress(const QString& itemId, qint64 playbackPositionTicks, double playedPercentage, bool played)
{
    if (itemId.isEmpty()) {
        return false;
    }

    bool updated = false;
    const auto normalizedTicks = std::max<qint64>(0, playbackPositionTicks);
    const auto normalizedPercentage = std::isfinite(playedPercentage) ? std::clamp(playedPercentage, 0.0, 100.0) : -1.0;
    for (int row = 0; row < rowCount(); ++row) {
        auto& item = m_items[static_cast<size_t>(row)];
        if (item.id != itemId) {
            continue;
        }

        bool changed = false;
        if (item.playbackPositionTicks != normalizedTicks) {
            item.playbackPositionTicks = normalizedTicks;
            changed = true;
        }
        if (normalizedPercentage >= 0.0 && std::abs(item.playedPercentage - normalizedPercentage) > 0.01) {
            item.playedPercentage = normalizedPercentage;
            changed = true;
        }
        if (item.played != played) {
            item.played = played;
            changed = true;
        }
        if (!changed) {
            continue;
        }

        const auto modelIndex = index(row, 0);
        emit dataChanged(modelIndex,
                         modelIndex,
                         { PlaybackPositionTicksRole, PlayedPercentageRole, PlayedRole });
        updated = true;
    }
    return updated;
}

void MediaItemListModel::clear()
{
    setItems({});
}

std::optional<MediaItem> MediaItemListModel::itemAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return m_items[static_cast<size_t>(row)];
}
