#pragma once
#include "services/iptv/IptvProbe.h"
#include "services/iptv/IptvStreamSelector.h"
#include <QJsonArray>

// Shared mutations for desktop and CLI. These functions never perform I/O.
namespace IptvStreamWorkflow {
void applyProbe(IptvChannel& channel, const QString& source, const IptvProbeResult& result);
bool applyPlayback(IptvChannel& channel, const QString& source, const QString& playback,
                   const QString& resolution, const QString& codec, const QString& bitrate);
void markFailed(IptvChannel& channel, const IptvStreamSelector::Selection& selection);
QJsonArray rows(const IptvChannel& channel, const QString& preference, const QString& current = {},
                const IptvStreamSelector::Selection* playing = nullptr);
}
