#include "viewmodels/PlaybackHistoryListModel.h"

#include <algorithm>
#include <ranges>

PlaybackHistoryListModel::PlaybackHistoryListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int PlaybackHistoryListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_items.size());
}

int PlaybackHistoryListModel::count() const
{
    return rowCount();
}

QVariant PlaybackHistoryListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& item = m_items[static_cast<size_t>(index.row())];
    switch (role) {
    case RecordIdRole:
        return item.id;
    case SourceTypeRole:
        return playbackHistorySourceToString(item.source);
    case ServiceNameRole:
        return item.serviceName;
    case TitleRole:
        return item.title;
    case SubtitleRole:
        return item.subtitle;
    case DisplayTargetRole:
        return item.displayTarget;
    case PlayedDateRole:
        return item.playedDate.toString(Qt::ISODate);
    case PlayedTimeRole:
        return item.playedAt.toLocalTime().time().toString(QStringLiteral("HH:mm"));
    case PositionSecondsRole:
        return QVariant::fromValue(item.positionSeconds);
    case DurationSecondsRole:
        return QVariant::fromValue(item.durationSeconds);
    case ProgressRole:
        return item.durationSeconds > 0
            ? std::clamp(static_cast<double>(item.positionSeconds) / static_cast<double>(item.durationSeconds), 0.0, 1.0)
            : 0.0;
    case CompletedRole:
        return item.completed;
    case PrivacyModeRole:
        return item.privacyMode;
    case AvailableRole:
        return item.available;
    default:
        return {};
    }
}

QHash<int, QByteArray> PlaybackHistoryListModel::roleNames() const
{
    return {
        { RecordIdRole, "recordId" },
        { SourceTypeRole, "sourceType" },
        { ServiceNameRole, "serviceName" },
        { TitleRole, "title" },
        { SubtitleRole, "subtitle" },
        { DisplayTargetRole, "displayTarget" },
        { PlayedDateRole, "playedDate" },
        { PlayedTimeRole, "playedTime" },
        { PositionSecondsRole, "positionSeconds" },
        { DurationSecondsRole, "durationSeconds" },
        { ProgressRole, "progress" },
        { CompletedRole, "completed" },
        { PrivacyModeRole, "privacyMode" },
        { AvailableRole, "available" },
    };
}

void PlaybackHistoryListModel::setItems(std::vector<PlaybackHistoryItem> items)
{
    beginResetModel();
    m_items = std::move(items);
    endResetModel();
    emit countChanged();
}

void PlaybackHistoryListModel::appendItems(std::vector<PlaybackHistoryItem> items)
{
    if (items.empty()) {
        return;
    }
    const auto first = rowCount();
    const auto last = first + static_cast<int>(items.size()) - 1;
    beginInsertRows({}, first, last);
    std::ranges::move(items, std::back_inserter(m_items));
    endInsertRows();
    emit countChanged();
}

void PlaybackHistoryListModel::clear()
{
    setItems({});
}

const PlaybackHistoryItem* PlaybackHistoryListModel::itemById(const QString& recordId) const
{
    const auto item = std::ranges::find(m_items, recordId, &PlaybackHistoryItem::id);
    return item == m_items.cend() ? nullptr : &*item;
}
