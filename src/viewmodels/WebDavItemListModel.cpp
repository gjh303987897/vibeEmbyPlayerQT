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
    case EncryptedHlsRole:
        return item.encryptedHls;
    case IdentifierPreviewRole:
        return item.identifierPreview;
    case SourceFileNameRole:
        return item.sourceFileName;
    case M3u8sMetadataPendingRole:
        return item.encryptedHls && !item.m3u8sMetadataResolved;
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
        { EncryptedHlsRole, "encryptedHls" },
        { IdentifierPreviewRole, "identifierPreview" },
        { SourceFileNameRole, "sourceFileName" },
        { M3u8sMetadataPendingRole, "m3u8sMetadataPending" },
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

void WebDavItemListModel::setM3u8sMetadata(const QUrl& url,
                                           QString identifierPreview,
                                           QString sourceFileName)
{
    for (auto& item : m_allItems) {
        if (item.url == url) {
            item.identifierPreview = identifierPreview;
            item.sourceFileName = sourceFileName;
            item.m3u8sMetadataResolved = true;
            break;
        }
    }

    for (int row = 0; row < rowCount(); ++row) {
        auto& item = m_items[static_cast<size_t>(row)];
        if (item.url != url) {
            continue;
        }
        item.identifierPreview = std::move(identifierPreview);
        item.sourceFileName = std::move(sourceFileName);
        item.m3u8sMetadataResolved = true;
        const auto changedIndex = index(row, 0);
        // Emitted even when both strings came back empty: that is the failed-read case,
        // and the pending flag still has to drop so the loading indicator disappears.
        emit dataChanged(changedIndex, changedIndex,
                         { IdentifierPreviewRole, SourceFileNameRole, M3u8sMetadataPendingRole });
        break;
    }
}

std::optional<WebDavItem> WebDavItemListModel::itemAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return m_items[static_cast<size_t>(row)];
}
