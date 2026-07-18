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
        AudioPlayableRole,
    };

    explicit WebDavItemListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setItems(std::vector<WebDavItem> items);
    void setDisplayMode(const QString& mode);
    QString displayMode() const;
    void setVideoMode(bool enabled);
    bool videoMode() const;
    bool audioMode() const;
    void clear();
    std::optional<WebDavItem> itemAt(int row) const;

signals:
    void countChanged();

private:
    void rebuildVisibleItems();

    std::vector<WebDavItem> m_allItems;
    std::vector<WebDavItem> m_items;
    QString m_displayMode { QStringLiteral("default") };
};
