#pragma once

#include <QString>

#include <expected>

class IptvPlaylistStore final {
public:
    static std::expected<QString, QString> importFile(const QString& sourcePath,
                                                      const QString& serviceId,
                                                      const QString& storageRoot = {});
};
