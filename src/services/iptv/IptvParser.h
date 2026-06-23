#pragma once

#include "models/IptvChannel.h"

#include <QString>

#include <expected>
#include <vector>

class IptvParser final {
public:
    static std::expected<std::vector<IptvChannel>, QString> parseFile(const QString& filePath);

private:
    static std::expected<QString, QString> readPlaylistText(const QString& filePath);
    static std::vector<IptvChannel> parsePlaylistText(const QString& text, const QString& fallbackName);
    static IptvChannel channelFromHlsManifest(const QString& filePath);
};
