#include "viewmodels/MediaLibraryListModel.h"

#include <utility>

MediaLibraryListModel::MediaLibraryListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int MediaLibraryListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return static_cast<int>(m_libraries.size());
}

QVariant MediaLibraryListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& library = m_libraries[static_cast<size_t>(index.row())];
    switch (role) {
    case IdRole:
        return library.id;
    case NameRole:
        return library.name;
    case CollectionTypeRole:
        return library.collectionType;
    case ItemTypeRole:
        return library.itemType;
    case ImageUrlRole:
        return library.imageUrl;
    case ChildCountRole:
        return library.childCount;
    default:
        return {};
    }
}

QHash<int, QByteArray> MediaLibraryListModel::roleNames() const
{
    return {
        { IdRole, "libraryId" },
        { NameRole, "name" },
        { CollectionTypeRole, "collectionType" },
        { ItemTypeRole, "itemType" },
        { ImageUrlRole, "imageUrl" },
        { ChildCountRole, "childCount" },
    };
}

void MediaLibraryListModel::setLibraries(std::vector<MediaLibrary> libraries)
{
    beginResetModel();
    m_libraries = std::move(libraries);
    endResetModel();
}

void MediaLibraryListModel::clear()
{
    setLibraries({});
}

std::optional<MediaLibrary> MediaLibraryListModel::libraryAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return m_libraries[static_cast<size_t>(row)];
}
