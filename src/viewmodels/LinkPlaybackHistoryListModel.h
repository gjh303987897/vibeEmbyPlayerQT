#pragma once

#include "models/LinkPlaybackHistoryItem.h"

#include <QAbstractListModel>

#include <vector>

class LinkPlaybackHistoryListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        RecordIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        DisplayAddressRole,
        PlayedDateRole,
        PlayedTimeRole,
        PrivacyModeRole,
    };

    explicit LinkPlaybackHistoryListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setItems(std::vector<LinkPlaybackHistoryItem> items);
    void clear();
    const LinkPlaybackHistoryItem* itemById(const QString& recordId) const;

signals:
    void countChanged();

private:
    std::vector<LinkPlaybackHistoryItem> m_items;
};
