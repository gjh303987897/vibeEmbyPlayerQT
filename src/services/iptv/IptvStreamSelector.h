#pragma once

#include "models/IptvChannel.h"

#include <QJsonArray>

namespace IptvStreamSelector {

struct Selection final {
    QString sourceUrl;
    QString playbackUrl;
    IptvStreamInfo info;
};

// Unknown/expired routes preserve playlist order. Known dimensions/bitrates
// take priority over missing metadata, which must not count as zero quality.
Selection select(const IptvChannel& channel, const QString& preference,
                 const QDateTime& now = QDateTime::currentDateTimeUtc());
std::vector<Selection> candidates(const IptvChannel& channel, const QString& preference,
                 const QDateTime& now = QDateTime::currentDateTimeUtc());
QString selectionId(const Selection& selection);
bool fresh(const IptvStreamInfo& info, const QDateTime& now = QDateTime::currentDateTimeUtc());
QJsonArray serialize(const std::vector<IptvStreamInfo>& info);
std::vector<IptvStreamInfo> deserialize(const QJsonArray& array);

}
