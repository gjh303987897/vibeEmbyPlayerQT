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
    case StreamCountRole:
        return channel.streamUrls.isEmpty() ? 1 : channel.streamUrls.size();
    case ResolutionRole:
        return channel.width > 0 && channel.height > 0
            ? QStringLiteral("%1x%2").arg(channel.width).arg(channel.height)
            : QString {};
    case CodecRole:
        return channel.codec;
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
        { StreamCountRole, "streamCount" },
        { ResolutionRole, "resolution" },
        { CodecRole, "codec" },
    };
}

int IptvChannelListModel::indexOfChannelId(const QString& channelId) const
{
    if (channelId.isEmpty()) {
        return -1;
    }

    for (auto index = 0; index < rowCount(); ++index) {
        if (m_channels[static_cast<size_t>(index)].id == channelId) {
            return index;
        }
    }
    return -1;
}

void IptvChannelListModel::setChannels(std::vector<IptvChannel> channels)
{
    beginResetModel();
    m_channels = std::move(channels);
    endResetModel();
    emit countChanged();
}

void IptvChannelListModel::updateChannelProbe(const QString& channelId,
                                              int width,
                                              int height,
                                              qint64 bitrate,
                                              const QString& codec,
                                              const QString& probeState)
{
    for (auto index = 0; index < rowCount(); ++index) {
        auto& channel = m_channels[static_cast<size_t>(index)];
        if (channel.id != channelId) continue;
        channel.width = width;
        channel.height = height;
        channel.bitrate = bitrate;
        channel.codec = codec;
        channel.probeState = probeState;
        emit dataChanged(this->index(index), this->index(index),
                         {ResolutionRole, CodecRole});
        return;
    }
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
