#include "services/webdav/TsslStore.h"

#include "services/webdav/AesGcmDecryptor.h"
#include "services/webdav/HlsManifestValidator.h"

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
#include <QStringDecoder>

#include <algorithm>

namespace {
constexpr qsizetype maximumTsslBytes = 64 * 1024 * 1024;
constexpr qsizetype maximumEntriesPerSection = 1'000'000;
constexpr qsizetype maximumPathCharacters = 4096;
constexpr qsizetype maximumSourceFileNameBytes = 4096;

QByteArray makeSourceFileNameAad(QByteArrayView identifier)
{
    QByteArray aad = QByteArrayLiteral("vibeEmbyPlayerQT/M3U8S/source-name/v1\n");
    aad.append(identifier.data(), identifier.size());
    return aad;
}

const QRegularExpression& sha256Pattern()
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-fA-F]{64}$"));
    return pattern;
}

const QRegularExpression& identifierPattern()
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{4096}$"));
    return pattern;
}

std::expected<QByteArray, QString> parseIdentifier(const QJsonValue& value)
{
    if (!value.isString()) {
        return std::unexpected(QStringLiteral("identifier must be a string"));
    }
    const auto text = value.toString();
    if (text.size() != TsslPackage::identifierLength ||
        !identifierPattern().match(text).hasMatch()) {
        return std::unexpected(QStringLiteral("identifier must contain exactly 4096 Base64URL characters"));
    }
    return text.toLatin1();
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

std::expected<QByteArray, QString> parseEncryptedSourceFileName(const QJsonValue& value)
{
    if (!value.isString()) {
        return std::unexpected(QStringLiteral("sourceName.encrypted must be a string"));
    }
    const auto encoded = value.toString().toLatin1();
    const auto decoded = QByteArray::fromBase64(
        encoded,
        QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
    if (decoded.size() <= 32 || decoded.size() > HlsManifestValidator::maximumEncryptedSourceNameBytes ||
        decoded.toBase64() != encoded) {
        return std::unexpected(QStringLiteral("sourceName.encrypted must be canonical base64 authenticated ciphertext"));
    }
    return decoded;
}

std::expected<QString, QString> decodeSourceFileName(QByteArrayView plaintext)
{
    if (plaintext.isEmpty() || plaintext.size() > maximumSourceFileNameBytes) {
        return std::unexpected(QStringLiteral("Decrypted source filename is empty or too long"));
    }
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString fileName = decoder.decode(plaintext);
    if (decoder.hasError() || fileName.isEmpty() || fileName == QStringLiteral(".") ||
        fileName == QStringLiteral("..") || fileName.contains(QLatin1Char('/')) ||
        fileName.contains(QLatin1Char('\\')) || QDir::isAbsolutePath(fileName)) {
        return std::unexpected(QStringLiteral("Decrypted source filename is not a safe basename"));
    }
    const auto containsControl = std::ranges::any_of(fileName, [](QChar character) {
        return character.unicode() < 0x20 || character.unicode() == 0x7f;
    });
    if (containsControl) {
        return std::unexpected(QStringLiteral("Decrypted source filename contains control characters"));
    }
    return fileName;
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

QByteArray TsslPackage::sourceFileNameAuthenticatedData(QByteArrayView identifier)
{
    return makeSourceFileNameAad(identifier);
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
    const auto versionValue = root.value(QStringLiteral("version"));
    const auto version = versionValue.isDouble() && versionValue.toDouble() == 2.0
        ? 2
        : versionValue.isDouble() && versionValue.toDouble() == 3.0 ? 3
        : versionValue.isDouble() && versionValue.toDouble() == 4.0 ? 4 : 0;
    if (root.value(QStringLiteral("format")).toString() != QStringLiteral("TSSL") ||
        !versionValue.isDouble() || (version != 2 && version != 3 && version != 4) ||
        root.value(QStringLiteral("algorithm")).toString() != QStringLiteral("AES-256-GCM")) {
        return std::unexpected(QStringLiteral("Unsupported TSSL format, version, or algorithm"));
    }

    auto identifier = parseIdentifier(root.value(QStringLiteral("identifier")));
    if (!identifier) {
        return std::unexpected(identifier.error());
    }

    auto rootDigest = parseDigest(root.value(QStringLiteral("rootManifestSha256")),
                                  QStringLiteral("rootManifestSha256"));
    if (!rootDigest) {
        return std::unexpected(rootDigest.error());
    }

    QString containerFormat;
    QByteArray containerIndexSha256;
    qint64 containerLength = 0;
    if (version == 4) {
        containerFormat = root.value(QStringLiteral("containerFormat")).toString();
        if (containerFormat != QStringLiteral("m3u8sp-tar-index-v1")) {
            return std::unexpected(QStringLiteral("TSSL v4 contains an unsupported container format"));
        }
        auto indexDigest = parseDigest(root.value(QStringLiteral("containerIndexSha256")),
                                       QStringLiteral("containerIndexSha256"));
        if (!indexDigest) {
            return std::unexpected(indexDigest.error());
        }
        containerIndexSha256 = std::move(*indexDigest);
        bool lengthOk = false;
        containerLength = root.value(QStringLiteral("containerLength")).toVariant().toLongLong(&lengthOk);
        if (!lengthOk || containerLength <= 0) {
            return std::unexpected(QStringLiteral("containerLength must be a positive integer"));
        }
    }

    QByteArray encryptedSourceFileName;
    QByteArray sourceFileNameKey;
    if (version == 3 || version == 4) {
        const auto sourceNameValue = root.value(QStringLiteral("sourceName"));
        if (!sourceNameValue.isObject()) {
            return std::unexpected(QStringLiteral("TSSL v3 requires sourceName metadata"));
        }
        const auto sourceName = sourceNameValue.toObject();
        auto parsedKey = parseKey(sourceName.value(QStringLiteral("key")), QStringLiteral("sourceName.key"));
        if (!parsedKey) {
            return std::unexpected(parsedKey.error());
        }
        auto parsedEncrypted = parseEncryptedSourceFileName(sourceName.value(QStringLiteral("encrypted")));
        if (!parsedEncrypted) {
            return std::unexpected(parsedEncrypted.error());
        }
        sourceFileNameKey = std::move(*parsedKey);
        encryptedSourceFileName = std::move(*parsedEncrypted);
    } else if (root.contains(QStringLiteral("sourceName"))) {
        return std::unexpected(QStringLiteral("TSSL v2 must not contain sourceName metadata"));
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

    TsslPackage package {
        .version = version,
        .containerFormat = std::move(containerFormat),
        .containerIndexSha256 = std::move(containerIndexSha256),
        .containerLength = containerLength,
        .identifier = std::move(*identifier),
        .rootManifestDigest = std::move(*rootDigest),
        .encryptedSourceFileName = std::move(encryptedSourceFileName),
        .sourceFileNameKey = std::move(sourceFileNameKey),
        .manifestDigests = std::move(*manifests),
        .segmentKeys = std::move(*segments),
        .resourceDigests = std::move(resources),
    };
    if (auto sourceFileName = package.decryptedSourceFileName(); !sourceFileName) {
        return std::unexpected(sourceFileName.error());
    }
    return package;
}

QByteArray TsslPackage::toJson() const
{
    QJsonObject root {
        { QStringLiteral("format"), QStringLiteral("TSSL") },
        { QStringLiteral("version"), version },
        { QStringLiteral("algorithm"), QStringLiteral("AES-256-GCM") },
        { QStringLiteral("identifier"), QString::fromLatin1(identifier) },
        { QStringLiteral("rootManifestSha256"), QString::fromLatin1(rootManifestDigest.toHex()) },
        { QStringLiteral("manifests"), digestEntries(manifestDigests) },
        { QStringLiteral("segments"), keyEntries(segmentKeys) },
        { QStringLiteral("resources"), digestEntries(resourceDigests) },
    };
    if (version == 4) {
        root.insert(QStringLiteral("containerFormat"), containerFormat);
        root.insert(QStringLiteral("containerIndexSha256"), QString::fromLatin1(containerIndexSha256.toHex()));
        root.insert(QStringLiteral("containerLength"), QJsonValue::fromVariant(containerLength));
    }
    if (version == 3 || version == 4) {
        root.insert(QStringLiteral("sourceName"), QJsonObject {
            { QStringLiteral("encrypted"), QString::fromLatin1(encryptedSourceFileName.toBase64()) },
            { QStringLiteral("key"), QString::fromLatin1(sourceFileNameKey.toBase64()) },
        });
    }
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

std::expected<std::optional<QString>, QString> TsslPackage::decryptedSourceFileName() const
{
    if (version == 2) {
        if (!encryptedSourceFileName.isEmpty() || !sourceFileNameKey.isEmpty()) {
            return std::unexpected(QStringLiteral("TSSL v2 contains unexpected source filename metadata"));
        }
        return std::optional<QString> {};
    }
    if ((version != 3 && version != 4) || identifier.size() != identifierLength || sourceFileNameKey.size() != 32 ||
        encryptedSourceFileName.size() <= 32 ||
        encryptedSourceFileName.size() > HlsManifestValidator::maximumEncryptedSourceNameBytes) {
        return std::unexpected(QStringLiteral("TSSL v3 source filename metadata is incomplete"));
    }
    const auto aad = sourceFileNameAuthenticatedData(identifier);
    auto plaintext = AesGcmDecryptor::decryptAuthenticatedData(
        encryptedSourceFileName,
        sourceFileNameKey,
        aad);
    if (!plaintext) {
        return std::unexpected(QStringLiteral("Source filename authentication failed: %1").arg(plaintext.error()));
    }
    auto fileName = decodeSourceFileName(*plaintext);
    plaintext->fill('\0');
    if (!fileName) {
        return std::unexpected(fileName.error());
    }
    return std::optional<QString> { std::move(*fileName) };
}

std::expected<void, QString> TsslStore::savePackage(const TsslPackage& package) const
{
    const auto encoded = package.toJson();
    auto validated = TsslPackage::parse(encoded);
    if (!validated) {
        return std::unexpected(validated.error());
    }
    if (auto ensured = ensureStorageDirectory(); !ensured) {
        return std::unexpected(ensured.error());
    }
    return writeSecretFile(packagePath(package.rootManifestDigest), encoded);
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

QString TsslStore::packageAlreadyExistsError()
{
    return QStringLiteral("TSSL package already exists");
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

namespace {

// Reads, parses and validates one package file. Anything that goes wrong comes
// back as an invalid TsslPackageInfo rather than an error, because the browser
// lists the broken file and explains why it cannot be used.
TsslPackageInfo readPackageInfo(const QFileInfo& fileInfo, const QByteArray& fileDigest)
{
    const auto appendInvalid = [&](QString error) {
        return TsslPackageInfo {
            .rootManifestDigest = fileDigest,
            .filePath = fileInfo.absoluteFilePath(),
            .modifiedAt = fileInfo.lastModified(),
            .fileSize = fileInfo.size(),
            .valid = false,
            .validationError = std::move(error),
        };
    };

    QFile file(fileInfo.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return appendInvalid(QStringLiteral("Unable to open local TSSL package: %1").arg(file.errorString()));
    }
    if (file.size() <= 0 || file.size() > maximumTsslBytes) {
        return appendInvalid(QStringLiteral("Local TSSL package is empty or exceeds the 64 MiB limit"));
    }
    auto package = TsslPackage::parse(file.readAll());
    if (!package) {
        return appendInvalid(package.error());
    }
    const auto expectedName = QString::fromLatin1(package->rootManifestDigest.toHex()) + QStringLiteral(".tssl");
    if (fileInfo.fileName().compare(expectedName, Qt::CaseInsensitive) != 0) {
        return appendInvalid(QStringLiteral("Local TSSL filename and manifest digest do not match"));
    }
    if (auto sourceFileName = package->decryptedSourceFileName(); !sourceFileName) {
        return appendInvalid(sourceFileName.error());
    }
    return TsslPackageInfo {
        .identifier = package->identifier,
        .rootManifestDigest = package->rootManifestDigest,
        .filePath = fileInfo.absoluteFilePath(),
        .modifiedAt = fileInfo.lastModified(),
        .fileSize = fileInfo.size(),
        .manifestCount = static_cast<int>(package->manifestDigests.size()),
        .segmentCount = static_cast<int>(package->segmentKeys.size()),
        .resourceCount = static_cast<int>(package->resourceDigests.size()),
        .valid = true,
    };
}

// Package files are named after their root digest, so the name alone identifies
// them and carries the digest the browser needs. Timestamps and sizes come from
// the directory entry, so listing stays cheap however large the packages are.
std::optional<TsslPackageSummary> readPackageSummary(const QFileInfo& fileInfo)
{
    const auto fileDigestText = fileInfo.completeBaseName();
    if (!sha256Pattern().match(fileDigestText).hasMatch()) {
        return std::nullopt;
    }
    return TsslPackageSummary {
        .rootManifestDigest = QByteArray::fromHex(fileDigestText.toLatin1()),
        .filePath = fileInfo.absoluteFilePath(),
        .modifiedAt = fileInfo.lastModified(),
        .fileSize = fileInfo.size(),
    };
}

} // namespace

std::expected<std::vector<TsslPackageInfo>, QString> TsslStore::listPackages() const
{
    std::vector<TsslPackageInfo> result;
    if (m_storageDirectory.isEmpty() || !QFileInfo::exists(m_storageDirectory)) {
        return result;
    }

    const QDir directory(m_storageDirectory);
    const auto files = directory.entryInfoList({ QStringLiteral("*.tssl") },
                                               QDir::Files | QDir::Readable,
                                               QDir::Time | QDir::Reversed);
    result.reserve(static_cast<size_t>(files.size()));
    for (const auto& fileInfo : files) {
        const auto summary = readPackageSummary(fileInfo);
        if (!summary) {
            continue;
        }
        result.push_back(readPackageInfo(fileInfo, summary->rootManifestDigest));
    }
    return result;
}

std::expected<std::vector<TsslPackageSummary>, QString> TsslStore::listPackageSummaries() const
{
    std::vector<TsslPackageSummary> result;
    if (m_storageDirectory.isEmpty() || !QFileInfo::exists(m_storageDirectory)) {
        return result;
    }

    const QDir directory(m_storageDirectory);
    const auto files = directory.entryInfoList({ QStringLiteral("*.tssl") },
                                               QDir::Files | QDir::Readable,
                                               QDir::Time | QDir::Reversed);
    result.reserve(static_cast<size_t>(files.size()));
    for (const auto& fileInfo : files) {
        if (const auto summary = readPackageSummary(fileInfo)) {
            result.push_back(*summary);
        }
    }
    return result;
}

std::expected<std::vector<TsslPackageInfo>, QString> TsslStore::packageInfosForPaths(const QStringList& paths) const
{
    std::vector<TsslPackageInfo> result;
    result.reserve(static_cast<size_t>(paths.size()));
    const QFileInfo storageDirectoryInfo(m_storageDirectory);
    const auto storageRoot = storageDirectoryInfo.exists() ? storageDirectoryInfo.canonicalFilePath() : QString {};
    for (const auto& path : paths) {
        const QFileInfo fileInfo(path);
        // Callers pass paths from listPackageSummaries(), but this is the one place
        // that reads package files from a name supplied from elsewhere, so re-check
        // that the file really is a package in this store before parsing it.
        if (storageRoot.isEmpty() || !fileInfo.exists() || !fileInfo.isFile()
            || fileInfo.dir().canonicalPath() != storageRoot) {
            return std::unexpected(QStringLiteral("TSSL package is not in the local store: %1").arg(path));
        }
        const auto summary = readPackageSummary(fileInfo);
        if (!summary) {
            return std::unexpected(QStringLiteral("TSSL package file name is not a manifest digest: %1").arg(path));
        }
        result.push_back(readPackageInfo(fileInfo, summary->rootManifestDigest));
    }
    return result;
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
    if (QFileInfo::exists(packagePath(package->rootManifestDigest))) {
        return std::unexpected(packageAlreadyExistsError());
    }
    if (auto saved = savePackage(*package); !saved) {
        return std::unexpected(saved.error());
    }
    return package->rootManifestDigest;
}

std::expected<void, QString> TsslStore::deleteByRootDigest(QByteArrayView digest) const
{
    if (digest.size() != 32) {
        return std::unexpected(QStringLiteral("Manifest digest must contain 32 bytes"));
    }
    const auto path = packagePath(digest);
    if (!QFileInfo::exists(path)) {
        return std::unexpected(QStringLiteral("No local TSSL package matches this manifest"));
    }
    if (!QFile::remove(path)) {
        return std::unexpected(QStringLiteral("Unable to delete the local TSSL package"));
    }
    return {};
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

std::expected<int, QString> TsslStore::exportByRootDigests(std::span<const QByteArray> digests,
                                                          const QString& destinationDirectory) const
{
    if (digests.empty()) {
        return std::unexpected(QStringLiteral("Select at least one TSSL package to export"));
    }

    const QFileInfo destinationInfo(destinationDirectory);
    if (!destinationInfo.exists() || !destinationInfo.isDir() || !destinationInfo.isWritable()) {
        return std::unexpected(QStringLiteral("Choose an available writable TSSL export folder"));
    }

    struct PendingExport final {
        QString path;
        QByteArray contents;
    };
    std::vector<QByteArray> uniqueDigests;
    std::vector<PendingExport> pending;
    uniqueDigests.reserve(digests.size());
    pending.reserve(digests.size());

    const QDir destination(destinationInfo.absoluteFilePath());
    for (const auto& digest : digests) {
        if (std::ranges::find(uniqueDigests, digest) != uniqueDigests.end()) {
            continue;
        }
        uniqueDigests.push_back(digest);

        auto package = packageForRootDigest(digest);
        if (!package) {
            return std::unexpected(package.error());
        }
        if (!*package) {
            return std::unexpected(QStringLiteral("No local TSSL package matches this manifest"));
        }

        pending.push_back(PendingExport {
            .path = destination.filePath(QString::fromLatin1(digest.toHex()) + QStringLiteral(".tssl")),
            .contents = (**package).toJson(),
        });
    }

    for (const auto& item : pending) {
        if (auto written = writeSecretFile(item.path, item.contents); !written) {
            return std::unexpected(written.error());
        }
    }
    return static_cast<int>(pending.size());
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
