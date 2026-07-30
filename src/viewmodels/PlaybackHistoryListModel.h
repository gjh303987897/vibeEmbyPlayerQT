#pragma once

#include "models/PlaybackHistoryItem.h"

#include <QAbstractListModel>

#include <vector>

class PlaybackHistoryListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        RecordIdRole = Qt::UserRole + 1,
        SourceTypeRole,
        ServiceNameRole,
        TitleRole,
        SubtitleRole,
        DisplayTargetRole,
        PlayedDateRole,
        PlayedTimeRole,
        PositionSecondsRole,
        DurationSecondsRole,
        ProgressRole,
        CompletedRole,
        PrivacyModeRole,
        AvailableRole,
    };

    explicit PlaybackHistoryListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setItems(std::vector<PlaybackHistoryItem> items);
    void appendItems(std::vector<PlaybackHistoryItem> items);
    void clear();
    const PlaybackHistoryItem* itemById(const QString& recordId) const;

signals:
    void countChanged();

private:
    std::vector<PlaybackHistoryItem> m_items;
};
