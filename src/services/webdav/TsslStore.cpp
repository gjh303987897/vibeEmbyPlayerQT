#include "services/webdav/TsslStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace {
constexpr qsizetype maximumTsslBytes = 64 * 1024 * 1024;
constexpr qsizetype maximumEntriesPerSection = 1'000'000;
constexpr qsizetype maximumPathCharacters = 4096;

const QRegularExpression& sha256Pattern()
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return pattern;
}

std::expected<QByteArray, QString> parseDigest(const QJsonValue& value, const QString& field)
{
    const auto text = value.toString();
    if (!sha256Pattern().match(text).hasMatch()) {
        return std::unexpected(QStringLiteral("%1 must be a 32-byte SHA-256 digest in hexadecimal").arg(field));
    }
    return QByteArray::fromHex(text.toLatin1());
}

std::expected<QString, QString> parsePath(const QJsonValue& value, const QString& field)
{
    const auto path = value.toString();
    if (path.isEmpty() || path.size() > maximumPathCharacters || path.contains(QLatin1Char('\\')) ||
        path.contains(QLatin1Char('?')) || path.contains(QLatin1Char('#')) || path.startsWith(QLatin1Char('/')) ||
        QDir::isAbsolutePath(path)) {
        return std::unexpected(QStringLiteral("%1 is not a safe package-relative path").arg(field));
    }
    const auto normalized = QDir::cleanPath(path);
    if (normalized != path || normalized == QStringLiteral(".") || normalized == QStringLiteral("..") ||
        normalized.startsWith(QStringLiteral("../"))) {
        return std::unexpected(QStringLiteral("%1 is not a canonical package-relative path").arg(field));
    }
    return path;
}

std::expected<QByteArray, QString> parseKey(const QJsonValue& value, const QString& field)
{
    const auto encoded = value.toString().toLatin1();
    if (encoded.size() != 44 || !encoded.endsWith('=') || encoded.contains('-') || encoded.contains('_')) {
        return std::unexpected(QStringLiteral("%1 must be standard base64 for a 32-byte key").arg(field));
    }
    const auto decoded = QByteArray::fromBase64(encoded);
    if (decoded.size() != 32 || decoded.toBase64() != encoded) {
        return std::unexpected(QStringLiteral("%1 must be standard base64 for a 32-byte key").arg(field));
    }
    return decoded;
}

template<typename ValueParser>
std::expected<QHash<QString, QByteArray>, QString> parseEntries(const QJsonValue& value,
                                                                const QString& section,
                                                                const QString& valueName,
                                                                ValueParser parser)
{
    if (!value.isArray()) {
        return std::unexpected(QStringLiteral("%1 must be an array").arg(section));
    }
    const auto array = value.toArray();
    if (array.size() > maximumEntriesPerSection) {
        return std::unexpected(QStringLiteral("%1 contains too many entries").arg(section));
    }

    QHash<QString, QByteArray> entries;
    entries.reserve(array.size());
    for (qsizetype index = 0; index < array.size(); ++index) {
        if (!array.at(index).isObject()) {
            return std::unexpected(QStringLiteral("%1[%2] must be an object").arg(section).arg(index));
        }
        const auto object = array.at(index).toObject();
        const auto fieldPrefix = QStringLiteral("%1[%2]").arg(section).arg(index);
        auto path = parsePath(object.value(QStringLiteral("path")), fieldPrefix + QStringLiteral(".path"));
        if (!path) {
            return std::unexpected(path.error());
        }
        if (entries.contains(*path)) {
            return std::unexpected(QStringLiteral("%1 contains duplicate path %2").arg(section, *path));
        }
        auto parsedValue = parser(object.value(valueName), fieldPrefix + QLatin1Char('.') + valueName);
        if (!parsedValue) {
            return std::unexpected(parsedValue.error());
        }
        entries.insert(*path, *parsedValue);
    }
    return entries;
}

QJsonArray digestEntries(const QHash<QString, QByteArray>& entries)
{
    QJsonArray result;
    auto paths = entries.keys();
    std::ranges::sort(paths);
    for (const auto& path : paths) {
        result.append(QJsonObject {
            { QStringLiteral("path"), path },
            { QStringLiteral("sha256"), QString::fromLatin1(entries.value(path).toHex()) },
        });
    }
    return result;
}

QJsonArray keyEntries(const QHash<QString, QByteArray>& entries)
{
    QJsonArray result;
    auto paths = entries.keys();
    std::ranges::sort(paths);
    for (const auto& path : paths) {
        result.append(QJsonObject {
            { QStringLiteral("path"), path },
            { QStringLiteral("key"), QString::fromLatin1(entries.value(path).toBase64()) },
        });
    }
    return result;
}

std::expected<void, QString> writeSecretFile(const QString& path, QByteArrayView contents)
{
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return std::unexpected(QStringLiteral("Unable to open TSSL destination: %1").arg(file.errorString()));
    }
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(contents.data(), contents.size()) != contents.size()) {
        return std::unexpected(QStringLiteral("Unable to write TSSL destination: %1").arg(file.errorString()));
    }
    if (!file.commit()) {
        return std::unexpected(QStringLiteral("Unable to commit TSSL destination: %1").arg(file.errorString()));
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return {};
}
}

std::expected<TsslPackage, QString> TsslPackage::parse(QByteArrayView document)
{
    if (document.isEmpty() || document.size() > maximumTsslBytes) {
        return std::unexpected(QStringLiteral("TSSL document is empty or exceeds the 64 MiB limit"));
    }

    QJsonParseError parseError;
    const auto json = QJsonDocument::fromJson(QByteArray(document.data(), document.size()), &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        return std::unexpected(QStringLiteral("Invalid TSSL JSON: %1").arg(parseError.errorString()));
    }
    const auto root = json.object();
    const auto version = root.value(QStringLiteral("version"));
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("TSSL") ||
        !version.isDouble() || version.toDouble() != 1.0 ||
        root.value(QStringLiteral("algorithm")).toString() != QStringLiteral("AES-256-GCM")) {
        return std::unexpected(QStringLiteral("Unsupported TSSL format, version, or algorithm"));
    }

    auto rootDigest = parseDigest(root.value(QStringLiteral("rootManifestSha256")),
                                  QStringLiteral("rootManifestSha256"));
    if (!rootDigest) {
        return std::unexpected(rootDigest.error());
    }
    auto manifests = parseEntries(root.value(QStringLiteral("manifests")),
                                  QStringLiteral("manifests"),
                                  QStringLiteral("sha256"),
                                  parseDigest);
    if (!manifests) {
        return std::unexpected(manifests.error());
    }
    auto segments = parseEntries(root.value(QStringLiteral("segments")),
                                 QStringLiteral("segments"),
                                 QStringLiteral("key"),
                                 parseKey);
    if (!segments) {
        return std::unexpected(segments.error());
    }

    QHash<QString, QByteArray> resources;
    if (root.contains(QStringLiteral("resources"))) {
        auto parsedResources = parseEntries(root.value(QStringLiteral("resources")),
                                            QStringLiteral("resources"),
                                            QStringLiteral("sha256"),
                                            parseDigest);
        if (!parsedResources) {
            return std::unexpected(parsedResources.error());
        }
        resources = std::move(*parsedResources);
    }

    if (segments->isEmpty()) {
        return std::unexpected(QStringLiteral("TSSL must contain at least one encrypted TS segment"));
    }
    for (auto it = segments->cbegin(); it != segments->cend(); ++it) {
        if (!it.key().endsWith(QStringLiteral(".ts"), Qt::CaseInsensitive)) {
            return std::unexpected(QStringLiteral("Encrypted segment path must end in .ts: %1").arg(it.key()));
        }
        if (manifests->contains(it.key()) || resources.contains(it.key())) {
            return std::unexpected(QStringLiteral("TSSL path is registered in more than one section: %1").arg(it.key()));
        }
    }
    for (auto it = manifests->cbegin(); it != manifests->cend(); ++it) {
        if ((!it.key().endsWith(QStringLiteral(".m3u8"), Qt::CaseInsensitive) &&
             !it.key().endsWith(QStringLiteral(".m3u8s"), Qt::CaseInsensitive)) ||
            resources.contains(it.key())) {
            return std::unexpected(QStringLiteral("Invalid or duplicate child manifest path: %1").arg(it.key()));
        }
    }

    return TsslPackage {
        .rootManifestDigest = std::move(*rootDigest),
        .manifestDigests = std::move(*manifests),
        .segmentKeys = std::move(*segments),
        .resourceDigests = std::move(resources),
    };
}

QByteArray TsslPackage::toJson() const
{
    const QJsonObject root {
        { QStringLiteral("format"), QStringLiteral("TSSL") },
        { QStringLiteral("version"), 1 },
        { QStringLiteral("algorithm"), QStringLiteral("AES-256-GCM") },
        { QStringLiteral("rootManifestSha256"), QString::fromLatin1(rootManifestDigest.toHex()) },
        { QStringLiteral("manifests"), digestEntries(manifestDigests) },
        { QStringLiteral("segments"), keyEntries(segmentKeys) },
        { QStringLiteral("resources"), digestEntries(resourceDigests) },
    };
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

TsslStore::TsslStore(QString storageDirectory)
    : m_storageDirectory(std::move(storageDirectory))
{
    if (m_storageDirectory.isEmpty()) {
        const auto dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!dataRoot.isEmpty()) {
            m_storageDirectory = QDir(dataRoot).filePath(QStringLiteral("tssl"));
        }
    }
}

std::expected<std::optional<TsslPackage>, QString> TsslStore::packageForRootDigest(QByteArrayView digest) const
{
    if (digest.size() != 32) {
        return std::unexpected(QStringLiteral("Manifest digest must contain 32 bytes"));
    }
    const auto path = packagePath(digest);
    if (!QFileInfo::exists(path)) {
        return std::optional<TsslPackage> {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("Unable to open local TSSL package: %1").arg(file.errorString()));
    }
    if (file.size() <= 0 || file.size() > maximumTsslBytes) {
        return std::unexpected(QStringLiteral("Local TSSL package is empty or exceeds the 64 MiB limit"));
    }
    auto package = TsslPackage::parse(file.readAll());
    if (!package) {
        return std::unexpected(package.error());
    }
    if (package->rootManifestDigest != QByteArray(digest.data(), digest.size())) {
        return std::unexpected(QStringLiteral("Local TSSL filename and manifest digest do not match"));
    }
    return std::optional<TsslPackage> { std::move(*package) };
}

std::expected<QByteArray, QString> TsslStore::restoreFromFile(const QString& sourcePath) const
{
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return std::unexpected(QStringLiteral("Unable to open TSSL file: %1").arg(source.errorString()));
    }
    if (source.size() <= 0 || source.size() > maximumTsslBytes) {
        return std::unexpected(QStringLiteral("TSSL file is empty or exceeds the 64 MiB limit"));
    }
    auto package = TsslPackage::parse(source.readAll());
    if (!package) {
        return std::unexpected(package.error());
    }
    if (auto ensured = ensureStorageDirectory(); !ensured) {
        return std::unexpected(ensured.error());
    }
    if (auto written = writeSecretFile(packagePath(package->rootManifestDigest), package->toJson()); !written) {
        return std::unexpected(written.error());
    }
    return package->rootManifestDigest;
}

std::expected<void, QString> TsslStore::exportByRootDigest(QByteArrayView digest, const QString& destinationPath) const
{
    auto package = packageForRootDigest(digest);
    if (!package) {
        return std::unexpected(package.error());
    }
    if (!*package) {
        return std::unexpected(QStringLiteral("No local TSSL package matches this manifest"));
    }
    return writeSecretFile(destinationPath, (**package).toJson());
}

QString TsslStore::storageDirectory() const
{
    return m_storageDirectory;
}

std::expected<void, QString> TsslStore::ensureStorageDirectory() const
{
    if (m_storageDirectory.isEmpty() || !QDir().mkpath(m_storageDirectory)) {
        return std::unexpected(QStringLiteral("Unable to create the local TSSL storage directory"));
    }
    QFile::setPermissions(m_storageDirectory,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return {};
}

QString TsslStore::packagePath(QByteArrayView digest) const
{
    const auto digestHex = QByteArray(digest.data(), digest.size()).toHex();
    return QDir(m_storageDirectory).filePath(QString::fromLatin1(digestHex) + QStringLiteral(".tssl"));
}
