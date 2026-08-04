#include "services/webdav/HlsManifestValidator.h"

#include <QDir>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUrl>

#include <algorithm>
#include <optional>
#include <string>

namespace HlsManifestValidator {
namespace {
constexpr qsizetype maximumManifestBytes = 4 * 1024 * 1024;
constexpr auto identifierPrefix = "#M3U8S-IDENTIFIER:";

bool isValidIdentifier(QByteArrayView identifier)
{
    if (identifier.size() != m3u8sIdentifierLength) {
        return false;
    }
    return std::ranges::all_of(identifier, [](char character) {
        return (character >= 'A' && character <= 'Z') ||
            (character >= 'a' && character <= 'z') ||
            (character >= '0' && character <= '9') ||
            character == '_' || character == '-';
    });
}

std::expected<void, QString> validateUri(const QString& uriText, const QString& manifestPath)
{
    const QUrl uri(uriText, QUrl::StrictMode);
    if (!uri.isValid() || uriText.isEmpty() || !uri.isRelative() || !uri.scheme().isEmpty() ||
        !uri.authority().isEmpty() || !uri.fragment().isEmpty() || uri.path().startsWith(QLatin1Char('/'))) {
        return std::unexpected(QStringLiteral("HLS URI must be package-relative: %1").arg(uriText));
    }

    const auto decodedPath = uri.path(QUrl::FullyDecoded);
    if (decodedPath.isEmpty() || decodedPath.contains(QLatin1Char('\\'))) {
        return std::unexpected(QStringLiteral("HLS URI contains an invalid path: %1").arg(uriText));
    }
    const auto slash = manifestPath.lastIndexOf(QLatin1Char('/'));
    const auto directory = slash >= 0 ? manifestPath.left(slash) : QString();
    const auto resolved = QDir::cleanPath(directory.isEmpty()
                                             ? decodedPath
                                             : directory + QLatin1Char('/') + decodedPath);
    if (resolved == QStringLiteral(".") || resolved == QStringLiteral("..") ||
        resolved.startsWith(QStringLiteral("../")) ||
        QDir::isAbsolutePath(resolved)) {
        return std::unexpected(QStringLiteral("HLS URI escapes the package root: %1").arg(uriText));
    }
    return {};
}
}

std::expected<void, QString> validate(QByteArrayView manifest, const QString& manifestPath)
{
    if (manifest.isEmpty() || manifest.size() > maximumManifestBytes) {
        return std::unexpected(QStringLiteral("HLS manifest is empty or exceeds the 4 MiB limit"));
    }

    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString text = decoder.decode(manifest);
    if (decoder.hasError() || text.startsWith(QChar::ByteOrderMark)) {
        return std::unexpected(QStringLiteral("HLS manifest must be valid UTF-8 without a byte-order mark"));
    }

    const auto lines = text.split(QLatin1Char('\n'));
    qsizetype firstContentLine = -1;
    for (qsizetype index = 0; index < lines.size(); ++index) {
        if (!lines.at(index).trimmed().isEmpty()) {
            firstContentLine = index;
            break;
        }
    }
    if (firstContentLine < 0 || lines.at(firstContentLine).trimmed() != QStringLiteral("#EXTM3U")) {
        return std::unexpected(QStringLiteral("HLS manifest does not begin with #EXTM3U"));
    }

    static const QRegularExpression uriAttribute(QStringLiteral("(?:^|[:,])URI=\"([^\"]*)\""));
    bool foundIdentifier = false;
    for (qsizetype index = firstContentLine + 1; index < lines.size(); ++index) {
        const auto line = lines.at(index).trimmed();
        if (line.isEmpty()) {
            continue;
        }
        if (line.startsWith(QStringLiteral("#EXT-X-KEY:")) ||
            line.startsWith(QStringLiteral("#EXT-X-SESSION-KEY:"))) {
            return std::unexpected(QStringLiteral("TSSL playlists must not contain HLS key tags"));
        }
        if (line.startsWith(QLatin1String(identifierPrefix))) {
            if (foundIdentifier) {
                return std::unexpected(QStringLiteral("M3U8S manifest contains more than one identifier"));
            }
            foundIdentifier = true;
            const auto identifier = line.sliced(QLatin1String(identifierPrefix).size()).toLatin1();
            if (!isValidIdentifier(identifier)) {
                return std::unexpected(QStringLiteral("M3U8S identifier must contain exactly 4096 Base64URL characters"));
            }
            continue;
        }
        if (!line.startsWith(QLatin1Char('#'))) {
            if (auto result = validateUri(line, manifestPath); !result) {
                return result;
            }
            continue;
        }

        auto matchIterator = uriAttribute.globalMatch(line);
        bool matchedUri = false;
        while (matchIterator.hasNext()) {
            matchedUri = true;
            const auto match = matchIterator.next();
            if (auto result = validateUri(match.captured(1), manifestPath); !result) {
                return result;
            }
        }
        if (line.contains(QStringLiteral("URI=")) && !matchedUri) {
            return std::unexpected(QStringLiteral("HLS URI attributes must use quoted-string syntax"));
        }
    }
    return {};
}

std::expected<QByteArray, QString> extractM3u8sIdentifier(QByteArrayView manifest)
{
    if (auto validated = validate(manifest); !validated) {
        return std::unexpected(validated.error());
    }

    const QByteArray bytes(manifest.data(), manifest.size());
    const auto lines = bytes.split('\n');
    std::optional<QByteArray> identifier;
    for (auto line : lines) {
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        if (!line.startsWith(identifierPrefix)) {
            continue;
        }
        if (identifier) {
            return std::unexpected(QStringLiteral("M3U8S manifest contains more than one identifier"));
        }
        identifier = line.sliced(static_cast<qsizetype>(std::char_traits<char>::length(identifierPrefix)));
    }
    if (!identifier) {
        return std::unexpected(QStringLiteral("M3U8S manifest does not contain an identifier"));
    }
    if (!isValidIdentifier(*identifier)) {
        return std::unexpected(QStringLiteral("M3U8S identifier must contain exactly 4096 Base64URL characters"));
    }
    return *identifier;
}

std::expected<QByteArray, QString> insertM3u8sIdentifier(QByteArrayView manifest,
                                                        QByteArrayView identifier)
{
    if (!isValidIdentifier(identifier)) {
        return std::unexpected(QStringLiteral("M3U8S identifier must contain exactly 4096 Base64URL characters"));
    }
    if (auto validated = validate(manifest); !validated) {
        return std::unexpected(validated.error());
    }

    const QByteArray bytes(manifest.data(), manifest.size());
    if (bytes.contains(identifierPrefix)) {
        return std::unexpected(QStringLiteral("HLS manifest already contains an M3U8S identifier"));
    }
    const auto firstLineEnd = bytes.indexOf('\n');
    if (firstLineEnd < 0) {
        return std::unexpected(QStringLiteral("HLS manifest must contain content after #EXTM3U"));
    }
    QByteArray result;
    result.reserve(bytes.size() + static_cast<qsizetype>(std::char_traits<char>::length(identifierPrefix)) +
                   identifier.size() + 1);
    result.append(bytes.first(firstLineEnd + 1));
    result.append(identifierPrefix);
    result.append(identifier.data(), identifier.size());
    result.append('\n');
    result.append(bytes.sliced(firstLineEnd + 1));
    return result;
}

}
