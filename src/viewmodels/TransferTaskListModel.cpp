#include "viewmodels/TransferTaskListModel.h"

#include <iterator>

TransferTaskListModel::TransferTaskListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int TransferTaskListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_tasks.size());
}

int TransferTaskListModel::count() const
{
    return rowCount();
}

QVariant TransferTaskListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& task = m_tasks[static_cast<size_t>(index.row())];
    switch (role) {
    case IdRole:
        return task.id;
    case TitleRole:
        return task.title;
    case DirectionRole:
        return task.direction;
    case StatusRole:
        return task.status;
    case DetailRole:
        return task.detail;
    case SourceRole:
        return task.source;
    case TargetRole:
        return task.target;
    case BytesDoneRole:
        return task.bytesDone;
    case BytesTotalRole:
        return task.bytesTotal;
    case BytesPerSecondRole:
        return task.bytesPerSecond;
    case ProgressRole:
        return task.progress;
    case CancellableRole:
        return task.cancellable;
    default:
        return {};
    }
}

QHash<int, QByteArray> TransferTaskListModel::roleNames() const
{
    return {
        { IdRole, "taskId" },
        { TitleRole, "title" },
        { DirectionRole, "direction" },
        { StatusRole, "status" },
        { DetailRole, "detail" },
        { SourceRole, "source" },
        { TargetRole, "target" },
        { BytesDoneRole, "bytesDone" },
        { BytesTotalRole, "bytesTotal" },
        { BytesPerSecondRole, "bytesPerSecond" },
        { ProgressRole, "progress" },
        { CancellableRole, "cancellable" },
    };
}

void TransferTaskListModel::setTasks(std::vector<TransferTask> tasks)
{
    beginResetModel();
    m_tasks = std::move(tasks);
    endResetModel();
    emit countChanged();
}

void TransferTaskListModel::appendTasks(std::vector<TransferTask> tasks)
{
    if (tasks.empty()) {
        return;
    }

    const auto firstRow = rowCount();
    const auto lastRow = firstRow + static_cast<int>(tasks.size()) - 1;
    beginInsertRows({}, firstRow, lastRow);
    m_tasks.insert(m_tasks.end(),
                   std::make_move_iterator(tasks.begin()),
                   std::make_move_iterator(tasks.end()));
    endInsertRows();
    emit countChanged();
}

void TransferTaskListModel::updateTask(const TransferTask& task)
{
    for (auto row = 0; row < rowCount(); ++row) {
        auto& existing = m_tasks[static_cast<size_t>(row)];
        if (existing.id != task.id) {
            continue;
        }
        existing = task;
        const auto modelIndex = index(row, 0);
        emit dataChanged(modelIndex, modelIndex);
        return;
    }
}
