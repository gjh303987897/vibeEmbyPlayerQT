#include "viewmodels/WebDavItemListModel.h"

WebDavItemListModel::WebDavItemListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int WebDavItemListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_items.size());
}

int WebDavItemListModel::count() const
{
    return rowCount();
}

QVariant WebDavItemListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& item = m_items[static_cast<size_t>(index.row())];
    switch (role) {
    case NameRole:
        return item.name;
    case UrlRole:
        return item.url;
    case RelativePathRole:
        return item.relativePath;
    case ContentTypeRole:
        return item.contentType;
    case LastModifiedRole:
        return item.lastModified;
    case SizeRole:
        return item.size;
    case DirectoryRole:
        return item.directory;
    case PlayableRole:
        return item.playable;
    default:
        return {};
    }
}

QHash<int, QByteArray> WebDavItemListModel::roleNames() const
{
    return {
        { NameRole, "name" },
        { UrlRole, "url" },
        { RelativePathRole, "relativePath" },
        { ContentTypeRole, "contentType" },
        { LastModifiedRole, "lastModified" },
        { SizeRole, "bytes" },
        { DirectoryRole, "directory" },
        { PlayableRole, "playable" },
    };
}

void WebDavItemListModel::setItems(std::vector<WebDavItem> items)
{
    beginResetModel();
    m_items = std::move(items);
    endResetModel();
    emit countChanged();
}

void WebDavItemListModel::clear()
{
    setItems({});
}

std::optional<WebDavItem> WebDavItemListModel::itemAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return m_items[static_cast<size_t>(row)];
}
