#include "viewmodels/PersonListModel.h"

#include <iterator>

PersonListModel::PersonListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int PersonListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : count();
}

int PersonListModel::count() const
{
    return static_cast<int>(m_people.size());
}

QVariant PersonListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& person = m_people[static_cast<size_t>(index.row())];
    switch (role) {
    case IdRole:
        return person.id;
    case NameRole:
        return person.name;
    case RoleNameRole:
        return person.role;
    case TypeRole:
        return person.type;
    case ImageUrlRole:
        return person.imageUrl;
    default:
        return {};
    }
}

QHash<int, QByteArray> PersonListModel::roleNames() const
{
    return {
        { IdRole, "personId" },
        { NameRole, "name" },
        { RoleNameRole, "roleName" },
        { TypeRole, "personType" },
        { ImageUrlRole, "imageUrl" },
    };
}

void PersonListModel::setPeople(std::vector<MediaPerson> people)
{
    beginResetModel();
    m_people = std::move(people);
    endResetModel();
    emit countChanged();
}

void PersonListModel::clear()
{
    setPeople({});
}
