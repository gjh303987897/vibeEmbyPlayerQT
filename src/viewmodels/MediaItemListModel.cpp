#include "viewmodels/MediaItemListModel.h"

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
        return item.imageUrl;
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
    };
}

void MediaItemListModel::setItems(std::vector<MediaItem> items)
{
    beginResetModel();
    m_items = std::move(items);
    endResetModel();
    emit countChanged();
}

void MediaItemListModel::appendItems(std::vector<MediaItem> items)
{
    if (items.empty()) {
        return;
    }

    const auto first = rowCount();
    const auto last = first + static_cast<int>(items.size()) - 1;
    beginInsertRows({}, first, last);
    m_items.insert(m_items.end(), std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
    endInsertRows();
    emit countChanged();
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
