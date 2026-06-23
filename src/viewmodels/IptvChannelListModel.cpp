#include "viewmodels/IptvChannelListModel.h"

IptvChannelListModel::IptvChannelListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int IptvChannelListModel::count() const
{
    return static_cast<int>(m_channels.size());
}

int IptvChannelListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return count();
}

QVariant IptvChannelListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& channel = m_channels[static_cast<size_t>(index.row())];
    switch (role) {
    case IdRole:
        return channel.id;
    case NameRole:
        return channel.name;
    case GroupNameRole:
        return channel.groupName;
    case LogoUrlRole:
        return channel.logoUrl;
    case StreamUrlRole:
        return channel.streamUrl;
    default:
        return {};
    }
}

QHash<int, QByteArray> IptvChannelListModel::roleNames() const
{
    return {
        { IdRole, "channelId" },
        { NameRole, "name" },
        { GroupNameRole, "groupName" },
        { LogoUrlRole, "logoUrl" },
        { StreamUrlRole, "streamUrl" },
    };
}

void IptvChannelListModel::setChannels(std::vector<IptvChannel> channels)
{
    beginResetModel();
    m_channels = std::move(channels);
    endResetModel();
    emit countChanged();
}

void IptvChannelListModel::clear()
{
    setChannels({});
}

std::optional<IptvChannel> IptvChannelListModel::channelAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return m_channels[static_cast<size_t>(row)];
}
