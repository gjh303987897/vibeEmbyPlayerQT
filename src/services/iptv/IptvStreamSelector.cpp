#include "services/iptv/IptvStreamSelector.h"

#include <QJsonObject>
#include <QCryptographicHash>
#include <QSet>
#include <algorithm>

namespace {
qint64 pixels(int width, int height)
{
    return static_cast<qint64>(std::max(0, width)) * std::max(0, height);
}

bool better(const IptvStreamInfo& left, const IptvStreamInfo& right, bool saveData)
{
    const auto leftPixels = pixels(left.width, left.height);
    const auto rightPixels = pixels(right.width, right.height);
    if ((leftPixels > 0) != (rightPixels > 0)) return leftPixels > 0;
    if (saveData && left.bitrate > 0 && right.bitrate > 0 && left.bitrate != right.bitrate)
        return left.bitrate < right.bitrate;
    if (leftPixels != rightPixels) return saveData ? leftPixels < rightPixels : leftPixels > rightPixels;
    if ((left.bitrate > 0) != (right.bitrate > 0)) return left.bitrate > 0;
    return saveData ? left.bitrate < right.bitrate : left.bitrate > right.bitrate;
}
}

bool IptvStreamSelector::fresh(const IptvStreamInfo& info, const QDateTime& now)
{
    if (!info.checkedAt.isValid()) return false;
    const auto age = info.checkedAt.secsTo(now);
    return age >= 0 && age < (info.available ? 6 * 60 * 60 : 60);
}

QString IptvStreamSelector::selectionId(const Selection& selection)
{
    auto bytes = selection.sourceUrl.toUtf8();
    bytes.append('\0');
    bytes.append(selection.playbackUrl.toUtf8());
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

std::vector<IptvStreamSelector::Selection> IptvStreamSelector::candidates(
    const IptvChannel& channel, const QString& preference, const QDateTime& now)
{
    struct Ranked { Selection selection; int availability; };
    std::vector<Ranked> ranked;
    const auto urls = channel.streamUrls.isEmpty() ? QStringList {channel.streamUrl} : channel.streamUrls;
    const auto saveData = preference == QStringLiteral("dataSaver");
    for (const auto& url : urls) {
        if (url.isEmpty()) continue;
        Selection candidate {.sourceUrl = url, .playbackUrl = url};
        const auto info = std::ranges::find(channel.streamInfo, url, &IptvStreamInfo::url);
        const auto isFresh = info != channel.streamInfo.end() && fresh(*info, now);
        if (isFresh) candidate.info = *info;
        const auto availability = isFresh ? (info->available ? 2 : 0) : 1;
        bool expanded = false;
        if (isFresh && !info->variants.empty()) {
            for (const auto& variant : info->variants) {
                if (variant.url.isEmpty()) continue;
                auto child = candidate;
                child.playbackUrl = variant.url;
                child.info.width = variant.width;
                child.info.height = variant.height;
                child.info.bitrate = variant.bitrate;
                child.info.codec = variant.codec;
                const auto age = variant.failedAt.secsTo(now);
                const auto recentlyFailed = variant.failedAt.isValid() && age >= 0 && age < 60;
                if (recentlyFailed) child.info.available = false;
                ranked.push_back({std::move(child), recentlyFailed ? 0 : availability});
                expanded = true;
            }
        }
        if (!expanded) ranked.push_back({std::move(candidate), availability});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [saveData](const Ranked& left, const Ranked& right) {
        if (left.availability != right.availability) return left.availability > right.availability;
        return left.availability == 2 && better(left.selection.info, right.selection.info, saveData);
    });
    std::vector<Selection> result;
    QSet<QString> seen;
    for (auto& entry : ranked) {
        if (seen.contains(entry.selection.playbackUrl)) continue;
        seen.insert(entry.selection.playbackUrl);
        result.push_back(std::move(entry.selection));
    }
    return result;
}

IptvStreamSelector::Selection IptvStreamSelector::select(
    const IptvChannel& channel, const QString& preference, const QDateTime& now)
{
    auto ranked = candidates(channel, preference, now);
    return ranked.empty() ? Selection {} : std::move(ranked.front());
}

QJsonArray IptvStreamSelector::serialize(const std::vector<IptvStreamInfo>& infos)
{
    QJsonArray array;
    for (const auto& info : infos) {
        QJsonArray variants;
        for (const auto& variant : info.variants) {
            variants.append(QJsonObject {{"url", variant.url}, {"width", variant.width},
                {"height", variant.height}, {"bitrate", variant.bitrate}, {"codec", variant.codec},
                {"failedAt", variant.failedAt.toUTC().toString(Qt::ISODateWithMs)}});
        }
        array.append(QJsonObject {{"url", info.url}, {"available", info.available},
            {"width", info.width}, {"height", info.height}, {"bitrate", info.bitrate},
            {"codec", info.codec}, {"state", info.state},
            {"checkedAt", info.checkedAt.toUTC().toString(Qt::ISODateWithMs)}, {"variants", variants}});
    }
    return array;
}

std::vector<IptvStreamInfo> IptvStreamSelector::deserialize(const QJsonArray& array)
{
    std::vector<IptvStreamInfo> result;
    for (const auto& value : array) {
        const auto object = value.toObject();
        const auto url = object.value("url").toString();
        if (url.isEmpty()) continue;
        IptvStreamInfo info {.url = url, .available = object.value("available").toBool(),
            .width = std::max(0, object.value("width").toInt()),
            .height = std::max(0, object.value("height").toInt()),
            .bitrate = std::max<qint64>(0, object.value("bitrate").toInteger()),
            .codec = object.value("codec").toString(), .state = object.value("state").toString(),
            .checkedAt = QDateTime::fromString(object.value("checkedAt").toString(), Qt::ISODateWithMs)};
        for (const auto& item : object.value("variants").toArray()) {
            const auto variant = item.toObject();
            if (variant.value("url").toString().isEmpty()) continue;
            info.variants.push_back({variant.value("url").toString(),
                std::max(0, variant.value("width").toInt()), std::max(0, variant.value("height").toInt()),
                std::max<qint64>(0, variant.value("bitrate").toInteger()), variant.value("codec").toString(),
                QDateTime::fromString(variant.value("failedAt").toString(), Qt::ISODateWithMs)});
        }
        result.push_back(std::move(info));
    }
    return result;
}
