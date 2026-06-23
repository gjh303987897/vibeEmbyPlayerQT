#include "services/iptv/IptvParser.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringConverter>
#include <QUrl>

namespace {
constexpr auto defaultGroupName = "Default";

QString decodeBytes(const QByteArray& bytes)
{
    auto decodeWith = [&bytes](const char* encoding) {
        QStringDecoder decoder(encoding);
        const QString text = decoder.decode(bytes);
        return std::pair { text, decoder.hasError() };
    };

    auto [utf8Text, utf8Error] = decodeWith("UTF-8");
    if (!utf8Error && !utf8Text.contains(QChar::ReplacementCharacter)) {
        return utf8Text;
    }

    auto [gb18030Text, gb18030Error] = decodeWith("GB18030");
    if (!gb18030Error && !gb18030Text.contains(QChar::ReplacementCharacter)) {
        return gb18030Text;
    }

    auto [gbkText, gbkError] = decodeWith("GBK");
    if (!gbkError) {
        return gbkText;
    }

    QStringDecoder systemDecoder(QStringConverter::System);
    const QString systemText = systemDecoder.decode(bytes);
    if (!systemDecoder.hasError()) {
        return systemText;
    }

    return QString::fromUtf8(bytes);
}

QString stableIdFor(const QString& seed)
{
    return QString::fromLatin1(QCryptographicHash::hash(seed.toUtf8(), QCryptographicHash::Sha256).toHex());
}

QString cleanName(QString value)
{
    value = value.trimmed();
    if ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
        || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))) {
        value = value.mid(1, value.size() - 2).trimmed();
    }
    return value;
}

QHash<QString, QString> parseAttributes(const QString& value)
{
    QHash<QString, QString> attributes;
    static const QRegularExpression attributePattern(QStringLiteral(R"m3u(([A-Za-z0-9_-]+)\s*=\s*"([^"]*)")m3u"));
    auto match = attributePattern.globalMatch(value);
    while (match.hasNext()) {
        const auto item = match.next();
        attributes.insert(item.captured(1).toLower(), item.captured(2).trimmed());
    }
    return attributes;
}

QString extinfTitle(const QString& line)
{
    const auto commaIndex = line.indexOf(QLatin1Char(','));
    if (commaIndex < 0 || commaIndex + 1 >= line.size()) {
        return {};
    }
    return cleanName(line.mid(commaIndex + 1));
}

bool isUrlLine(const QString& line)
{
    if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) {
        return false;
    }

    const auto url = QUrl(line);
    return url.isValid() || line.endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive);
}

bool looksLikeHlsManifest(const QString& text)
{
    return text.contains(QStringLiteral("#EXT-X-STREAM-INF"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("#EXT-X-TARGETDURATION"), Qt::CaseInsensitive)
        || text.contains(QStringLiteral("#EXT-X-MEDIA-SEQUENCE"), Qt::CaseInsensitive);
}
}

std::expected<std::vector<IptvChannel>, QString> IptvParser::parseFile(const QString& filePath)
{
    const auto text = readPlaylistText(filePath);
    if (!text) {
        return std::unexpected(text.error());
    }

    if (looksLikeHlsManifest(*text)) {
        return std::vector<IptvChannel> { channelFromHlsManifest(filePath) };
    }

    auto channels = parsePlaylistText(*text, QFileInfo(filePath).completeBaseName());
    if (channels.empty()) {
        const auto trimmed = text->trimmed();
        if (isUrlLine(trimmed)) {
            IptvChannel channel;
            channel.id = stableIdFor(filePath + QLatin1Char('|') + trimmed);
            channel.name = QFileInfo(filePath).completeBaseName();
            channel.groupName = QString::fromLatin1(defaultGroupName);
            channel.streamUrl = trimmed;
            channels.push_back(std::move(channel));
        }
    }

    if (channels.empty()) {
        return std::unexpected(QStringLiteral("No IPTV channels were found in this playlist"));
    }
    return channels;
}

std::expected<QString, QString> IptvParser::readPlaylistText(const QString& filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("Unable to open IPTV playlist file"));
    }

    const auto bytes = file.readAll();
    if (bytes.isEmpty()) {
        return std::unexpected(QStringLiteral("IPTV playlist file is empty"));
    }
    return decodeBytes(bytes);
}

std::vector<IptvChannel> IptvParser::parsePlaylistText(const QString& text, const QString& fallbackName)
{
    std::vector<IptvChannel> channels;
    IptvChannel pending;
    bool hasPending = false;
    int sortOrder = 0;

    const auto lines = text.split(QRegularExpression(QStringLiteral("\\r\\n|\\n|\\r")), Qt::SkipEmptyParts);
    for (const auto& rawLine : lines) {
        const auto line = rawLine.trimmed();
        if (line.isEmpty()) {
            continue;
        }

        if (line.startsWith(QStringLiteral("#EXTINF"), Qt::CaseInsensitive)) {
            const auto attributes = parseAttributes(line);
            pending = {};
            pending.name = attributes.value(QStringLiteral("tvg-name"));
            if (pending.name.isEmpty()) {
                pending.name = extinfTitle(line);
            }
            if (pending.name.isEmpty()) {
                pending.name = fallbackName;
            }
            pending.groupName = attributes.value(QStringLiteral("group-title"));
            if (pending.groupName.isEmpty()) {
                pending.groupName = QString::fromLatin1(defaultGroupName);
            }
            pending.logoUrl = attributes.value(QStringLiteral("tvg-logo"));
            pending.sortOrder = sortOrder;
            hasPending = true;
            continue;
        }

        if (!isUrlLine(line)) {
            continue;
        }

        auto channel = hasPending ? pending : IptvChannel {};
        channel.streamUrl = line;
        channel.sortOrder = sortOrder++;
        if (channel.name.isEmpty()) {
            channel.name = QFileInfo(QUrl(line).path()).completeBaseName();
            if (channel.name.isEmpty()) {
                channel.name = fallbackName.isEmpty() ? QStringLiteral("IPTV Channel") : fallbackName;
            }
        }
        if (channel.groupName.isEmpty()) {
            channel.groupName = QString::fromLatin1(defaultGroupName);
        }
        channel.id = stableIdFor(channel.name + QLatin1Char('|') + channel.groupName + QLatin1Char('|') + channel.streamUrl);
        channels.push_back(std::move(channel));
        hasPending = false;
    }

    return channels;
}

IptvChannel IptvParser::channelFromHlsManifest(const QString& filePath)
{
    const auto info = QFileInfo(filePath);
    IptvChannel channel;
    channel.id = stableIdFor(info.absoluteFilePath());
    channel.name = info.completeBaseName().isEmpty() ? QStringLiteral("IPTV Channel") : info.completeBaseName();
    channel.groupName = QString::fromLatin1(defaultGroupName);
    channel.streamUrl = QUrl::fromLocalFile(info.absoluteFilePath()).toString();
    return channel;
}
