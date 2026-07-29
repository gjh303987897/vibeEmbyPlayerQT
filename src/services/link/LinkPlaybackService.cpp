#include "services/link/LinkPlaybackService.h"

#include <QFileInfo>

namespace {
constexpr qsizetype maximumPlaybackUrlLength = 16 * 1024;
}

LinkPlaybackService::UrlResult LinkPlaybackService::resolvePlaybackUrl(const QString& input)
{
    const auto normalizedInput = input.trimmed();
    if (normalizedInput.isEmpty()) {
        return std::unexpected(LinkPlaybackError::Empty);
    }
    if (normalizedInput.size() > maximumPlaybackUrlLength) {
        return std::unexpected(LinkPlaybackError::TooLong);
    }

    QUrl url(normalizedInput, QUrl::StrictMode);
    if (!url.isValid() || url.isRelative()) {
        return std::unexpected(LinkPlaybackError::Invalid);
    }

    const auto scheme = url.scheme().toLower();
    if (scheme != QStringLiteral("http") && scheme != QStringLiteral("https")) {
        return std::unexpected(LinkPlaybackError::UnsupportedScheme);
    }
    if (url.host().isEmpty()) {
        return std::unexpected(LinkPlaybackError::MissingHost);
    }
    if (!url.userName().isEmpty() || !url.password().isEmpty()) {
        return std::unexpected(LinkPlaybackError::EmbeddedCredentials);
    }

    url.setScheme(scheme);
    url.setFragment({});
    return url;
}

QString LinkPlaybackService::displayName(const QUrl& url)
{
    const auto fileName = QFileInfo(url.path()).fileName().trimmed();
    if (!fileName.isEmpty()) {
        return fileName;
    }
    const auto host = url.host().trimmed();
    return host.isEmpty() ? QStringLiteral("Link Playback") : host;
}
