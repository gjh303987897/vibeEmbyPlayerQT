#include "services/iptv/IptvStreamWorkflow.h"
#include <QJsonObject>
#include <algorithm>

namespace {
IptvStreamInfo& route(IptvChannel& channel, const QString& source)
{
    auto it = std::ranges::find(channel.streamInfo, source, &IptvStreamInfo::url);
    if (it == channel.streamInfo.end()) {
        channel.streamInfo.push_back({.url = source});
        return channel.streamInfo.back();
    }
    return *it;
}
}
void IptvStreamWorkflow::applyProbe(IptvChannel& channel, const QString& source, const IptvProbeResult& result)
{
    auto& info = route(channel, source);
    const auto now = QDateTime::currentDateTimeUtc();
    if ((info.state != "playback" && info.state != "playback-failed") || !IptvStreamSelector::fresh(info)) {
        info = {.url = source, .available = result.available, .width = result.width, .height = result.height,
            .bitrate = result.bitrate, .codec = result.codec, .state = result.state, .checkedAt = now,
            .variants = result.variants, .qualityCheckedAt = result.detailed ? now : QDateTime {}};
    } else if (result.detailed) {
        info.qualityCheckedAt = now;
        for (auto& variant : info.variants) {
            if (variant.width > 0 && variant.height > 0) continue;
            const auto found = std::ranges::find(result.variants, variant.url, &IptvStreamVariant::url);
            if (found != result.variants.end()) {
                const auto failed = variant.failedAt;
                variant = *found;
                variant.failedAt = failed;
            }
        }
    }
}
bool IptvStreamWorkflow::applyPlayback(IptvChannel& channel, const QString& source, const QString& playback,
                                      const QString& resolution, const QString& codec, const QString& bitrate)
{
    const auto parts = resolution.split('x');
    const auto width = parts.value(0).toInt(), height = parts.value(1).toInt();
    if (width <= 0 || height <= 0) return false;
    auto normalized = bitrate.trimmed().toLower(); normalized.remove(',');
    qint64 bits = normalized.endsWith("mbps") ? static_cast<qint64>(normalized.chopped(4).toDouble() * 1000000) :
        normalized.endsWith("kbps") ? static_cast<qint64>(normalized.chopped(4).toDouble() * 1000) : normalized.toLongLong();
    auto& info = route(channel, source);
    info.available = true; info.width = width; info.height = height;
    if (bits > 0) info.bitrate = bits;
    if (!codec.isEmpty()) info.codec = codec;
    info.state = "playback"; info.checkedAt = QDateTime::currentDateTimeUtc();
    for (auto& variant : info.variants) if (variant.url == playback) {
        variant.failedAt = {}; variant.width = width; variant.height = height;
        if (bits > 0) variant.bitrate = bits;
        if (!codec.isEmpty()) variant.codec = codec;
    }
    channel.width = width; channel.height = height; channel.bitrate = std::max<qint64>(0, bits);
    channel.codec = codec; channel.probeState = "playback";
    return true;
}
void IptvStreamWorkflow::markFailed(IptvChannel& channel, const IptvStreamSelector::Selection& selection)
{
    auto& info = route(channel, selection.sourceUrl);
    const auto now = QDateTime::currentDateTimeUtc();
    if (selection.playbackUrl != selection.sourceUrl) {
        const auto variant = std::ranges::find(info.variants, selection.playbackUrl, &IptvStreamVariant::url);
        if (variant != info.variants.end()) { variant->failedAt = now; return; }
    }
    info.available = false; info.state = "playback-failed"; info.checkedAt = now;
}
QJsonArray IptvStreamWorkflow::rows(const IptvChannel& channel, const QString& preference, const QString& current,
                                    const IptvStreamSelector::Selection* playing)
{
    QJsonArray result;
    auto choices = IptvStreamSelector::candidates(channel, preference);
    // A master playlist may already be playing when a late probe discovers
    // child variants. Keep the actual selection visible without replacing it.
    if (playing && !current.isEmpty() && std::ranges::none_of(choices,[&](const auto& item) { return IptvStreamSelector::selectionId(item) == current; })) {
        auto actual = *playing;
        const auto info = std::ranges::find(channel.streamInfo,actual.sourceUrl,&IptvStreamInfo::url);
        if (info != channel.streamInfo.end()) actual.info = *info;
        choices.push_back(std::move(actual));
    }
    for (const auto& candidate : choices) {
        const auto& info = candidate.info;
        const auto id = IptvStreamSelector::selectionId(candidate);
        result += QJsonObject {{"id",id},{"line",channel.streamUrls.indexOf(candidate.sourceUrl) + 1},
            {"current",id == current},{"width",info.width},{"height",info.height},{"bitrate",info.bitrate},
            {"codec",info.codec},{"available",info.available},{"state",info.state.isEmpty() ? "pending" : info.state},
            {"checkedAt",info.checkedAt.toString(Qt::ISODateWithMs)}};
    }
    return result;
}
