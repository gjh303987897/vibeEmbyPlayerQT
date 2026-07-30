#include "viewmodels/LinkPlaybackHistoryListModel.h"

#include <algorithm>
#include <ranges>

LinkPlaybackHistoryListModel::LinkPlaybackHistoryListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int LinkPlaybackHistoryListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_items.size());
}

int LinkPlaybackHistoryListModel::count() const
{
    return rowCount();
}

QVariant LinkPlaybackHistoryListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& item = m_items[static_cast<size_t>(index.row())];
    switch (role) {
    case RecordIdRole:
        return item.id;
    case DisplayNameRole:
        return item.displayName;
    case DisplayAddressRole:
        return item.displayAddress;
    case PlayedDateRole:
        return item.playedDate.toString(Qt::ISODate);
    case PlayedTimeRole:
        return item.playedAt.toLocalTime().time().toString(QStringLiteral("HH:mm"));
    case PrivacyModeRole:
        return item.privacyMode;
    default:
        return {};
    }
}

QHash<int, QByteArray> LinkPlaybackHistoryListModel::roleNames() const
{
    return {
        { RecordIdRole, "recordId" },
        { DisplayNameRole, "displayName" },
        { DisplayAddressRole, "displayAddress" },
        { PlayedDateRole, "playedDate" },
        { PlayedTimeRole, "playedTime" },
        { PrivacyModeRole, "privacyMode" },
    };
}

void LinkPlaybackHistoryListModel::setItems(std::vector<LinkPlaybackHistoryItem> items)
{
    beginResetModel();
    m_items = std::move(items);
    endResetModel();
    emit countChanged();
}

void LinkPlaybackHistoryListModel::clear()
{
    setItems({});
}

const LinkPlaybackHistoryItem* LinkPlaybackHistoryListModel::itemById(const QString& recordId) const
{
    const auto item = std::ranges::find(m_items, recordId, &LinkPlaybackHistoryItem::id);
    return item == m_items.cend() ? nullptr : &*item;
}
