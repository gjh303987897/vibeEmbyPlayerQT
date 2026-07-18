#include "viewmodels/WebDavItemListModel.h"

#include <algorithm>
#include <iterator>
#include <QString>

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
    case AudioPlayableRole:
        return item.audioPlayable;
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
        { AudioPlayableRole, "audioPlayable" },
    };
}

void WebDavItemListModel::setItems(std::vector<WebDavItem> items)
{
    m_allItems = std::move(items);
    rebuildVisibleItems();
}

void WebDavItemListModel::setDisplayMode(const QString& mode)
{
    const auto normalized = mode.compare(QStringLiteral("video"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("video")
        : mode.compare(QStringLiteral("audio"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("audio")
            : QStringLiteral("default");
    if (m_displayMode == normalized) {
        return;
    }
    m_displayMode = normalized;
    rebuildVisibleItems();
}

QString WebDavItemListModel::displayMode() const
{
    return m_displayMode;
}

void WebDavItemListModel::setVideoMode(bool enabled)
{
    setDisplayMode(enabled ? QStringLiteral("video") : QStringLiteral("default"));
}

bool WebDavItemListModel::videoMode() const
{
    return m_displayMode == QStringLiteral("video");
}

bool WebDavItemListModel::audioMode() const
{
    return m_displayMode == QStringLiteral("audio");
}

void WebDavItemListModel::rebuildVisibleItems()
{
    beginResetModel();
    m_items.clear();
    if (m_displayMode == QStringLiteral("video")) {
        m_items.reserve(m_allItems.size());
        std::ranges::copy_if(m_allItems,
                             std::back_inserter(m_items),
                             [](const WebDavItem& item) {
            return item.directory || item.playable;
        });
    } else if (m_displayMode == QStringLiteral("audio")) {
        m_items.reserve(m_allItems.size());
        std::ranges::copy_if(m_allItems,
                             std::back_inserter(m_items),
                             [](const WebDavItem& item) {
            return item.audioPlayable;
        });
    } else {
        m_items = m_allItems;
    }
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
