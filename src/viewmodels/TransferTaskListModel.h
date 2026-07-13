#pragma once

#include "models/TransferTask.h"

#include <QAbstractListModel>

#include <vector>

class TransferTaskListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TitleRole,
        DirectionRole,
        StatusRole,
        DetailRole,
        SourceRole,
        TargetRole,
        BytesDoneRole,
        BytesTotalRole,
        BytesPerSecondRole,
        ProgressRole,
        CancellableRole,
    };

    explicit TransferTaskListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setTasks(std::vector<TransferTask> tasks);
    void appendTasks(std::vector<TransferTask> tasks);
    void updateTask(const TransferTask& task);

signals:
    void countChanged();

private:
    std::vector<TransferTask> m_tasks;
};
