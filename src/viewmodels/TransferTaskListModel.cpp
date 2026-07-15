#include "viewmodels/TransferTaskListModel.h"

#include <algorithm>
#include <iterator>

namespace {
QString normalizedStatusFilter(const QString& filter)
{
    if (filter == QStringLiteral("completed") ||
        filter == QStringLiteral("incomplete") ||
        filter == QStringLiteral("failed") ||
        filter == QStringLiteral("canceled")) {
        return filter;
    }
    return QStringLiteral("all");
}
}

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

QString TransferTaskListModel::statusFilter() const
{
    return m_statusFilter;
}

void TransferTaskListModel::setStatusFilter(const QString& filter)
{
    const auto normalized = normalizedStatusFilter(filter);
    if (m_statusFilter == normalized) {
        return;
    }

    beginResetModel();
    m_statusFilter = normalized;
    rebuildVisibleTasks();
    endResetModel();
    emit countChanged();
    emit statusFilterChanged();
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
    case ParentIdRole:
        return task.parentId;
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
    case AverageBytesPerSecondRole:
        return task.averageBytesPerSecond;
    case BytesRemainingRole:
        return task.bytesRemaining;
    case ProgressRole:
        return task.progress;
    case FileCountRole:
        return task.fileCount;
    case CompletedFileCountRole:
        return task.completedFileCount;
    case IsGroupRole:
        return task.isGroup;
    case CancellableRole:
        return task.cancellable;
    case CanPauseRole:
        return task.canPause;
    case CanResumeRole:
        return task.canResume;
    case RetryableRole:
        return task.retryable;
    default:
        return {};
    }
}

QHash<int, QByteArray> TransferTaskListModel::roleNames() const
{
    return {
        { IdRole, "taskId" },
        { ParentIdRole, "parentId" },
        { TitleRole, "title" },
        { DirectionRole, "direction" },
        { StatusRole, "status" },
        { DetailRole, "detail" },
        { SourceRole, "source" },
        { TargetRole, "target" },
        { BytesDoneRole, "bytesDone" },
        { BytesTotalRole, "bytesTotal" },
        { BytesPerSecondRole, "bytesPerSecond" },
        { AverageBytesPerSecondRole, "averageBytesPerSecond" },
        { BytesRemainingRole, "bytesRemaining" },
        { ProgressRole, "progress" },
        { FileCountRole, "fileCount" },
        { CompletedFileCountRole, "completedFileCount" },
        { IsGroupRole, "isGroup" },
        { CancellableRole, "cancellable" },
        { CanPauseRole, "canPause" },
        { CanResumeRole, "canResume" },
        { RetryableRole, "retryable" },
    };
}

void TransferTaskListModel::setTasks(std::vector<TransferTask> tasks)
{
    beginResetModel();
    m_sourceTasks = std::move(tasks);
    rebuildVisibleTasks();
    endResetModel();
    emit countChanged();
}

void TransferTaskListModel::appendTasks(std::vector<TransferTask> tasks)
{
    if (tasks.empty()) {
        return;
    }

    std::vector<TransferTask> visibleTasks;
    visibleTasks.reserve(tasks.size());
    std::ranges::copy_if(tasks,
                         std::back_inserter(visibleTasks),
                         [this](const TransferTask& task) { return acceptsTask(task); });
    m_sourceTasks.insert(m_sourceTasks.end(),
                         std::make_move_iterator(tasks.begin()),
                         std::make_move_iterator(tasks.end()));
    if (visibleTasks.empty()) {
        return;
    }

    const auto firstRow = rowCount();
    const auto lastRow = firstRow + static_cast<int>(visibleTasks.size()) - 1;
    beginInsertRows({}, firstRow, lastRow);
    m_tasks.insert(m_tasks.end(),
                   std::make_move_iterator(visibleTasks.begin()),
                   std::make_move_iterator(visibleTasks.end()));
    endInsertRows();
    emit countChanged();
}

void TransferTaskListModel::updateTask(const TransferTask& task)
{
    const auto source = std::ranges::find_if(m_sourceTasks, [&task](const TransferTask& existing) {
        return existing.id == task.id;
    });
    if (source == m_sourceTasks.end()) {
        return;
    }

    const auto visible = std::ranges::find_if(m_tasks, [&task](const TransferTask& existing) {
        return existing.id == task.id;
    });
    const auto wasVisible = visible != m_tasks.end();
    *source = task;
    const auto isVisible = acceptsTask(task);

    if (wasVisible && isVisible) {
        const auto row = static_cast<int>(std::distance(m_tasks.begin(), visible));
        *visible = task;
        const auto modelIndex = index(row, 0);
        emit dataChanged(modelIndex, modelIndex);
        return;
    }
    if (!wasVisible && !isVisible) {
        return;
    }

    beginResetModel();
    rebuildVisibleTasks();
    endResetModel();
    emit countChanged();
}

bool TransferTaskListModel::acceptsTask(const TransferTask& task) const
{
    if (m_statusFilter == QStringLiteral("completed")) {
        return task.status == QStringLiteral("done");
    }
    if (m_statusFilter == QStringLiteral("incomplete")) {
        return task.status == QStringLiteral("queued") ||
            task.status == QStringLiteral("running") ||
            task.status == QStringLiteral("paused");
    }
    if (m_statusFilter == QStringLiteral("failed")) {
        return task.status == QStringLiteral("failed");
    }
    if (m_statusFilter == QStringLiteral("canceled")) {
        return task.status == QStringLiteral("canceled");
    }
    return true;
}

void TransferTaskListModel::rebuildVisibleTasks()
{
    m_tasks.clear();
    m_tasks.reserve(m_sourceTasks.size());
    std::ranges::copy_if(m_sourceTasks,
                         std::back_inserter(m_tasks),
                         [this](const TransferTask& task) { return acceptsTask(task); });
}
