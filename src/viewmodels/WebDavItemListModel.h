#pragma once

#include "models/WebDavItem.h"

#include <QAbstractListModel>

#include <vector>

class WebDavItemListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        NameRole = Qt::UserRole + 1,
        UrlRole,
        RelativePathRole,
        ContentTypeRole,
        LastModifiedRole,
        SizeRole,
        DirectoryRole,
        PlayableRole,
    };

    explicit WebDavItemListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setItems(std::vector<WebDavItem> items);
    void clear();
    std::optional<WebDavItem> itemAt(int row) const;

signals:
    void countChanged();

private:
    std::vector<WebDavItem> m_items;
};
