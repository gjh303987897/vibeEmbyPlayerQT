#include "viewmodels/LocalMediaRootListModel.h"

#include <utility>

LocalMediaRootListModel::LocalMediaRootListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int LocalMediaRootListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_roots.size());
}

int LocalMediaRootListModel::count() const
{
    return rowCount();
}

QVariant LocalMediaRootListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& root = m_roots[static_cast<size_t>(index.row())];
    switch (role) {
    case IdRole:
        return root.id;
    case NameRole:
        return root.name;
    case PathRole:
        return root.path;
    case AvailableRole:
        return root.available;
    default:
        return {};
    }
}

QHash<int, QByteArray> LocalMediaRootListModel::roleNames() const
{
    return {
        { IdRole, "rootId" },
        { NameRole, "name" },
        { PathRole, "path" },
        { AvailableRole, "available" },
    };
}

void LocalMediaRootListModel::setRoots(std::vector<LocalMediaRoot> roots)
{
    beginResetModel();
    m_roots = std::move(roots);
    endResetModel();
    emit countChanged();
}

void LocalMediaRootListModel::clear()
{
    setRoots({});
}

std::optional<LocalMediaRoot> LocalMediaRootListModel::rootAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return m_roots[static_cast<size_t>(row)];
}
