#pragma once

#include "models/LocalMediaItem.h"

#include <QAbstractListModel>

#include <optional>
#include <vector>

class LocalMediaItemListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        PathRole,
        LastModifiedRole,
        SizeRole,
        DirectoryRole,
    };

    explicit LocalMediaItemListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setItems(std::vector<LocalMediaItem> items);
    void clear();
    std::optional<LocalMediaItem> itemAt(int row) const;

signals:
    void countChanged();

private:
    std::vector<LocalMediaItem> m_items;
};
