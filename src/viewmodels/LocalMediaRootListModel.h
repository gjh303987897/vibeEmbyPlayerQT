#pragma once

#include "models/LocalMediaRoot.h"

#include <QAbstractListModel>

#include <optional>
#include <vector>

class LocalMediaRootListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        PathRole,
        AvailableRole,
    };

    explicit LocalMediaRootListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRoots(std::vector<LocalMediaRoot> roots);
    void clear();
    std::optional<LocalMediaRoot> rootAt(int row) const;

signals:
    void countChanged();

private:
    std::vector<LocalMediaRoot> m_roots;
};
