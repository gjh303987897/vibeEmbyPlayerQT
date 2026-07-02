#include "viewmodels/DailyUsageStatsListModel.h"

DailyUsageStatsListModel::DailyUsageStatsListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int DailyUsageStatsListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_stats.size());
}

int DailyUsageStatsListModel::count() const
{
    return rowCount();
}

QVariant DailyUsageStatsListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& stat = m_stats[static_cast<size_t>(index.row())];
    switch (role) {
    case DateRole:
        return stat.date;
    case ServiceIdRole:
        return stat.serviceId;
    case ServiceNameRole:
        return stat.serviceName;
    case ServiceTypeRole:
        return stat.serviceType;
    case WatchSecondsRole:
        return QVariant::fromValue(stat.watchSeconds);
    case NetworkBytesInRole:
        return QVariant::fromValue(stat.networkBytesIn);
    case NetworkBytesOutRole:
        return QVariant::fromValue(stat.networkBytesOut);
    case NetworkBytesTotalRole:
        return QVariant::fromValue(stat.networkBytesIn + stat.networkBytesOut);
    case PrivacyModeRole:
        return stat.privacyMode;
    default:
        return {};
    }
}

QHash<int, QByteArray> DailyUsageStatsListModel::roleNames() const
{
    return {
        { DateRole, "date" },
        { ServiceIdRole, "serviceId" },
        { ServiceNameRole, "serviceName" },
        { ServiceTypeRole, "serviceType" },
        { WatchSecondsRole, "watchSeconds" },
        { NetworkBytesInRole, "networkBytesIn" },
        { NetworkBytesOutRole, "networkBytesOut" },
        { NetworkBytesTotalRole, "networkBytesTotal" },
        { PrivacyModeRole, "privacyMode" },
    };
}

void DailyUsageStatsListModel::setStats(std::vector<DailyUsageStat> stats)
{
    beginResetModel();
    m_stats = std::move(stats);
    endResetModel();
    emit countChanged();
}

void DailyUsageStatsListModel::clear()
{
    setStats({});
}

const std::vector<DailyUsageStat>& DailyUsageStatsListModel::stats() const
{
    return m_stats;
}
