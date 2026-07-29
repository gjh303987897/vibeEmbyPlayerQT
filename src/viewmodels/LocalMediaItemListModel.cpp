#include "viewmodels/LocalMediaItemListModel.h"

#include <utility>

LocalMediaItemListModel::LocalMediaItemListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int LocalMediaItemListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_items.size());
}

int LocalMediaItemListModel::count() const
{
    return rowCount();
}

QVariant LocalMediaItemListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& item = m_items[static_cast<size_t>(index.row())];
    switch (role) {
    case NameRole:
        return item.name;
    case PathRole:
        return item.path;
    case LastModifiedRole:
        return item.lastModified;
    case SizeRole:
        return item.size;
    case DirectoryRole:
        return item.directory;
    default:
        return {};
    }
}

QHash<int, QByteArray> LocalMediaItemListModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { PathRole, "path" },
        { LastModifiedRole, "lastModified" },
        { SizeRole, "bytes" },
        { DirectoryRole, "directory" },
    };
}

void LocalMediaItemListModel::setItems(std::vector<LocalMediaItem> items)
{
    beginResetModel();
    m_items = std::move(items);
    endResetModel();
    emit countChanged();
}

void LocalMediaItemListModel::clear()
{
    setItems({});
}

std::optional<LocalMediaItem> LocalMediaItemListModel::itemAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return m_items[static_cast<size_t>(row)];
}
