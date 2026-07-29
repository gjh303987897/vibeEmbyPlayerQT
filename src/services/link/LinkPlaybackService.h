#pragma once

#include <QString>
#include <QUrl>

#include <expected>

enum class LinkPlaybackError {
    Empty,
    TooLong,
    Invalid,
    UnsupportedScheme,
    MissingHost,
    EmbeddedCredentials,
};

class LinkPlaybackService final {
public:
    using UrlResult = std::expected<QUrl, LinkPlaybackError>;

    static UrlResult resolvePlaybackUrl(const QString& input);
    static QString displayName(const QUrl& url);
};
