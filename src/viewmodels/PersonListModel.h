#pragma once

#include "models/MediaPerson.h"

#include <QAbstractListModel>
#include <vector>

class PersonListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        RoleNameRole,
        TypeRole,
        ImageUrlRole,
    };

    explicit PersonListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setPeople(std::vector<MediaPerson> people);
    void clear();

signals:
    void countChanged();

private:
    std::vector<MediaPerson> m_people;
};
