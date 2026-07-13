#include "viewmodels/ScheduledPlaybackTaskListModel.h"

#include <utility>

ScheduledPlaybackTaskListModel::ScheduledPlaybackTaskListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int ScheduledPlaybackTaskListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_tasks.size());
}

int ScheduledPlaybackTaskListModel::count() const
{
    return rowCount();
}

QVariant ScheduledPlaybackTaskListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& task = m_tasks[static_cast<size_t>(index.row())];
    switch (role) {
    case IdRole:
        return task.id;
    case ServerIdRole:
        return task.serverId;
    case ServerNameRole:
        return task.serverName;
    case UsernameRole:
        return task.username;
    case StartTimeRole:
        return task.startTime;
    case DurationMinutesRole:
        return task.durationMinutes;
    case EnabledRole:
        return task.enabled;
    case LastRunDateRole:
        return task.lastRunDate;
    default:
        return {};
    }
}

QHash<int, QByteArray> ScheduledPlaybackTaskListModel::roleNames() const
{
    return {
        { IdRole, "taskId" },
        { ServerIdRole, "serverId" },
        { ServerNameRole, "serverName" },
        { UsernameRole, "username" },
        { StartTimeRole, "startTime" },
        { DurationMinutesRole, "durationMinutes" },
        { EnabledRole, "enabled" },
        { LastRunDateRole, "lastRunDate" },
    };
}

void ScheduledPlaybackTaskListModel::setTasks(std::vector<ScheduledPlaybackTask> tasks)
{
    beginResetModel();
    m_tasks = std::move(tasks);
    endResetModel();
    emit countChanged();
}

std::optional<ScheduledPlaybackTask> ScheduledPlaybackTaskListModel::taskAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return m_tasks[static_cast<size_t>(row)];
}
