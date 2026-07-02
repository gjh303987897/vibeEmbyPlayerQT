#pragma once

#include "models/DailyUsageStat.h"

#include <QAbstractListModel>

#include <optional>
#include <vector>

class DailyUsageStatsListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        DateRole = Qt::UserRole + 1,
        ServiceIdRole,
        ServiceNameRole,
        ServiceTypeRole,
        WatchSecondsRole,
        NetworkBytesInRole,
        NetworkBytesOutRole,
        NetworkBytesTotalRole,
        PrivacyModeRole,
    };

    explicit DailyUsageStatsListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setStats(std::vector<DailyUsageStat> stats);
    void clear();
    const std::vector<DailyUsageStat>& stats() const;

signals:
    void countChanged();

private:
    std::vector<DailyUsageStat> m_stats;
};
