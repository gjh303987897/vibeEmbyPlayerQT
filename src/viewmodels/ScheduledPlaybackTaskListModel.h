#pragma once

#include "models/ScheduledPlaybackTask.h"

#include <QAbstractListModel>

#include <optional>
#include <vector>

class ScheduledPlaybackTaskListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        ServerIdRole,
        ServerNameRole,
        UsernameRole,
        StartTimeRole,
        DurationMinutesRole,
        EnabledRole,
        LastRunDateRole,
        PrivateModeRole,
    };

    explicit ScheduledPlaybackTaskListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setTasks(std::vector<ScheduledPlaybackTask> tasks);
    std::optional<ScheduledPlaybackTask> taskAt(int row) const;

signals:
    void countChanged();

private:
    std::vector<ScheduledPlaybackTask> m_tasks;
};
