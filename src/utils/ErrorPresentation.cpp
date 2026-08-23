#include "utils/ErrorPresentation.h"

#include <QRegularExpression>

#include <initializer_list>

namespace {
bool containsAny(const QString& value, std::initializer_list<QStringView> needles)
{
    for (const auto needle : needles) {
        if (value.contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}
}

QString ErrorPresentation::sanitizeDetails(QString details)
{
    details.replace(QRegularExpression(QStringLiteral("(?i)(authorization\\s*[:=]\\s*)(?:(?:bearer|basic)\\s+)?[^\\s,;]+")), QStringLiteral("\\1[REDACTED]"));
    details.replace(QRegularExpression(QStringLiteral("(?i)(cookie\\s*[:=]\\s*|api[-_ ]?key\\s*[:=]\\s*|token\\s*[:=]\\s*)[^\\s,;]+")), QStringLiteral("\\1[REDACTED]"));
    details.replace(QRegularExpression(QStringLiteral("(?i)(https?://)([^/@\\s]+):([^/@\\s]+)@([^/\\s]+)")), QStringLiteral("\\1[REDACTED]@[REDACTED]"));
    details.replace(QRegularExpression(QStringLiteral("(?i)([?&](?:api[_-]?key|token|password|passwd|secret)=)[^&\\s]+")), QStringLiteral("\\1[REDACTED]"));
    constexpr qsizetype maxDetailsLength = 11970;
    if (details.size() > maxDetailsLength) {
        details.truncate(maxDetailsLength);
        details.append(QStringLiteral("\\n[details truncated]"));
    }
    return details.trimmed();
}

AppErrorPresentation ErrorPresentation::fromMessage(const QString& message)
{
    const auto details = sanitizeDetails(message);
    AppErrorPresentation result;
    result.details = details;
    result.titleKey = QStringLiteral("error.genericTitle");
    result.summaryKey = QStringLiteral("error.genericSummary");
    result.hintKey = QStringLiteral("error.genericHint");

    if (containsAny(message, { QStringView(u"no decodable video"), QStringView(u"MPEG-TS"), QStringView(u"H.264"), QStringView(u"H.265"), QStringView(u"codec") })) {
        result.code = AppErrorCode::Packaging;
        result.titleKey = QStringLiteral("error.packagingTitle");
        result.summaryKey = QStringLiteral("error.packagingSummary");
        result.hintKey = QStringLiteral("error.packagingHint");
    } else if (containsAny(message, { QStringView(u"401"), QStringView(u"403"), QStringView(u"unauthorized"), QStringView(u"forbidden"), QStringView(u"password is required"), QStringView(u"credential") })) {
        result.code = AppErrorCode::Authentication;
        result.titleKey = QStringLiteral("error.authenticationTitle");
        result.summaryKey = QStringLiteral("error.authenticationSummary");
        result.hintKey = QStringLiteral("error.authenticationHint");
    } else if (containsAny(message, { QStringView(u"timed out"), QStringView(u"timeout"), QStringView(u"network request"), QStringView(u"connection"), QStringView(u"HTTP 5"), QStringView(u"HTTP 4") })) {
        result.code = AppErrorCode::Network;
        result.titleKey = QStringLiteral("error.networkTitle");
        result.summaryKey = QStringLiteral("error.networkSummary");
        result.hintKey = QStringLiteral("error.networkHint");
    } else if (containsAny(message, { QStringView(u"file"), QStringView(u"folder"), QStringView(u"directory"), QStringView(u"path"), QStringView(u"permission"), QStringView(u"not found") })) {
        result.code = AppErrorCode::File;
        result.titleKey = QStringLiteral("error.fileTitle");
        result.summaryKey = QStringLiteral("error.fileSummary");
        result.hintKey = QStringLiteral("error.fileHint");
    } else if (containsAny(message, { QStringView(u"playback"), QStringView(u"playable"), QStringView(u"media item"), QStringView(u"decode") })) {
        result.code = AppErrorCode::Playback;
        result.titleKey = QStringLiteral("error.playbackTitle");
        result.summaryKey = QStringLiteral("error.playbackSummary");
        result.hintKey = QStringLiteral("error.playbackHint");
    } else if (containsAny(message, { QStringView(u"required"), QStringView(u"select "), QStringView(u"invalid"), QStringView(u"unsupported") })) {
        result.code = AppErrorCode::Validation;
        result.titleKey = QStringLiteral("error.validationTitle");
        result.summaryKey = QStringLiteral("error.validationSummary");
        result.hintKey = QStringLiteral("error.validationHint");
    }
    return result;
}
