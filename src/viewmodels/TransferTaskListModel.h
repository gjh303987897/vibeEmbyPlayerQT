#pragma once

#include "models/TransferTask.h"

#include <QAbstractListModel>

#include <vector>

class TransferTaskListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString statusFilter READ statusFilter WRITE setStatusFilter NOTIFY statusFilterChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        ParentIdRole,
        TitleRole,
        DirectionRole,
        StatusRole,
        DetailRole,
        SourceRole,
        TargetRole,
        BytesDoneRole,
        BytesTotalRole,
        BytesPerSecondRole,
        AverageBytesPerSecondRole,
        BytesRemainingRole,
        ProgressRole,
        FileCountRole,
        CompletedFileCountRole,
        IsGroupRole,
        CancellableRole,
        CanPauseRole,
        CanResumeRole,
        RetryableRole,
    };

    explicit TransferTaskListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QString statusFilter() const;
    void setStatusFilter(const QString& filter);
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setTasks(std::vector<TransferTask> tasks);
    void appendTasks(std::vector<TransferTask> tasks);
    void updateTask(const TransferTask& task);

signals:
    void countChanged();
    void statusFilterChanged();

private:
    bool acceptsTask(const TransferTask& task) const;
    void rebuildVisibleTasks();

    QString m_statusFilter { QStringLiteral("all") };
    std::vector<TransferTask> m_sourceTasks;
    std::vector<TransferTask> m_tasks;
};
