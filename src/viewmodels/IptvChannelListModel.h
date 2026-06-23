#pragma once

#include "models/IptvChannel.h"

#include <QAbstractListModel>

#include <optional>
#include <vector>

class IptvChannelListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        NameRole,
        GroupNameRole,
        LogoUrlRole,
        StreamUrlRole
    };

    explicit IptvChannelListModel(QObject* parent = nullptr);

    int count() const;
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setChannels(std::vector<IptvChannel> channels);
    void clear();
    std::optional<IptvChannel> channelAt(int row) const;

signals:
    void countChanged();

private:
    std::vector<IptvChannel> m_channels;
};
