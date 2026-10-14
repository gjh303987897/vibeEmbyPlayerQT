#include "TsslBackupArchive.h"

#include "services/webdav/TsslStore.h"

#include <miniz.h>

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <limits>
#include <vector>

namespace {
constexpr qint64 maximumManifestBytes = 4LL * 1024 * 1024;
constexpr qint64 maximumDirectoryBytes = 8LL * 1024 * 1024;
const QString manifestName = QStringLiteral("manifest.json");

bool isCanceled(const std::atomic_bool* canceled)
{
    return canceled && canceled->load(std::memory_order_relaxed);
}

QString canceledError()
{
    return QStringLiteral("TSSL backup canceled");
}

struct IoContext final {
    QIODevice* device;
    const std::atomic_bool* canceled;
    qint64 limit;
};

size_t readCallback(void* opaque, mz_uint64 offset, void* buffer, size_t size)
{
    auto& context = *static_cast<IoContext*>(opaque);
    if (isCanceled(context.canceled) || offset > static_cast<mz_uint64>(context.limit)
        || size > static_cast<mz_uint64>(context.limit) - offset
        || size > static_cast<size_t>(std::numeric_limits<qint64>::max())) return 0;
    if (!context.device->seek(static_cast<qint64>(offset))) return 0;
    const auto read = context.device->read(static_cast<char*>(buffer), static_cast<qint64>(size));
    return read < 0 ? 0 : static_cast<size_t>(read);
}

size_t writeCallback(void* opaque, mz_uint64 offset, const void* buffer, size_t size)
{
    auto& context = *static_cast<IoContext*>(opaque);
    if (isCanceled(context.canceled) || offset > static_cast<mz_uint64>(context.limit)
        || size > static_cast<mz_uint64>(context.limit) - offset
        || size > static_cast<size_t>(std::numeric_limits<qint64>::max())) return 0;
    if (!context.device->seek(static_cast<qint64>(offset))) return 0;
    const auto written = context.device->write(static_cast<const char*>(buffer), static_cast<qint64>(size));
    return written < 0 ? 0 : static_cast<size_t>(written);
}

struct Zip final {
    mz_zip_archive archive {};
    ~Zip() { if (archive.m_pState) mz_zip_end(&archive); }
    QString error(const QString& operation, const std::atomic_bool* canceled) const
    {
        if (isCanceled(canceled)) return canceledError();
        return QStringLiteral("%1: %2").arg(operation,
            QString::fromLatin1(mz_zip_get_error_string(mz_zip_peek_last_error(const_cast<mz_zip_archive*>(&archive)))));
    }
};

bool isDigest(const QString& value)
{
    static const QRegularExpression expression(QStringLiteral("\\A[0-9a-f]{64}\\z"));
    return expression.match(value).hasMatch();
}

bool isPackageName(const QString& value)
{
    return value.size() == 69 && value.endsWith(QStringLiteral(".tssl")) && isDigest(value.left(64));
}

std::expected<QByteArray, QString> readPackage(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return std::unexpected(QStringLiteral("Cannot read TSSL package: %1").arg(file.errorString()));
    const auto size = file.size();
    if (size <= 0 || size > TsslBackupArchive::maxPackageBytes)
        return std::unexpected(QStringLiteral("TSSL package is empty or exceeds the 64 MiB limit"));
    auto data = file.read(size + 1);
    if (data.size() != size || !file.atEnd()) return std::unexpected(QStringLiteral("TSSL package changed or could not be read completely"));
    return data;
}

// miniz normally allocates the entire central directory during init. Reject a
// giant directory before init, so metadata in an untrusted ZIP cannot consume
// memory proportional to the 2 GiB archive limit. ZIP64 is unnecessary at our
// size/count limits and is intentionally excluded from backup format v1.
std::expected<void, QString> checkDirectoryBounds(QFile& file)
{
    const auto tailSize = std::min<qint64>(file.size(), 65535 + 22);
    if (!file.seek(file.size() - tailSize)) return std::unexpected(QStringLiteral("Cannot read ZIP directory"));
    const auto tail = file.read(tailSize);
    for (qsizetype position = tail.size() - 22; position >= 0; --position) {
        const auto* bytes = reinterpret_cast<const uchar*>(tail.constData() + position);
        if (qFromLittleEndian<quint32>(bytes) != 0x06054b50) continue;
        const auto comment = qFromLittleEndian<quint16>(bytes + 20);
        if (position + 22 + comment != tail.size()) continue;
        const auto count = qFromLittleEndian<quint16>(bytes + 10);
        const auto directorySize = qFromLittleEndian<quint32>(bytes + 12);
        const auto directoryOffset = qFromLittleEndian<quint32>(bytes + 16);
        const auto endOffset = file.size() - tailSize + position;
        if (qFromLittleEndian<quint16>(bytes + 4) != 0 || qFromLittleEndian<quint16>(bytes + 6) != 0
            || count == 0xffff || directorySize == 0xffffffff || directoryOffset == 0xffffffff
            || count != qFromLittleEndian<quint16>(bytes + 8)
            || count < 2 || count > TsslBackupArchive::maximumEntries + 1
            || directorySize > maximumDirectoryBytes
            || static_cast<qint64>(directoryOffset) + directorySize > endOffset)
            return std::unexpected(QStringLiteral("ZIP directory exceeds backup limits or uses an unsupported layout"));
        return {};
    }
    return std::unexpected(QStringLiteral("ZIP directory is missing or corrupt"));
}

struct Entry final {
    mz_uint index;
    QString name;
    qint64 size;
};

struct ManifestEntry final {
    QString digest;
    QString sha256;
    qint64 size;
};

std::expected<QHash<QString, ManifestEntry>, QString> parseManifest(const QByteArray& data, int packageCount)
{
    QJsonParseError jsonError;
    const auto document = QJsonDocument::fromJson(data, &jsonError);
    if (jsonError.error != QJsonParseError::NoError || !document.isObject())
        return std::unexpected(QStringLiteral("Backup manifest is not valid JSON"));
    const auto object = document.object();
    if (object.value(QStringLiteral("format")).toString() != QStringLiteral("TSSL-BACKUP")
        || object.value(QStringLiteral("version")).toDouble(-1) != 1
        || object.value(QStringLiteral("count")).toDouble(-1) != packageCount
        || !object.value(QStringLiteral("packages")).isArray())
        return std::unexpected(QStringLiteral("Unsupported backup manifest or package count mismatch"));
    const auto packages = object.value(QStringLiteral("packages")).toArray();
    if (packages.size() != packageCount) return std::unexpected(QStringLiteral("Backup manifest package count mismatch"));
    QHash<QString, ManifestEntry> result;
    for (const auto& value : packages) {
        const auto entry = value.toObject();
        const auto name = entry.value(QStringLiteral("file")).toString();
        const auto digest = entry.value(QStringLiteral("rootManifestSha256")).toString();
        const auto hash = entry.value(QStringLiteral("sha256")).toString();
        const auto sizeValue = entry.value(QStringLiteral("size"));
        const auto size = sizeValue.toDouble(-1);
        if (!value.isObject() || !isPackageName(name) || !isDigest(digest) || !isDigest(hash)
            || name != digest + QStringLiteral(".tssl") || result.contains(name)
            || !sizeValue.isDouble() || size <= 0 || size > TsslBackupArchive::maxPackageBytes
            || size != static_cast<qint64>(size))
            return std::unexpected(QStringLiteral("Backup manifest contains an invalid or duplicate package entry"));
        result.insert(name, {digest, hash, static_cast<qint64>(size)});
    }
    return result;
}
}

bool TsslBackupArchive::isArchiveName(const QString& name)
{
    static const QRegularExpression expression(QStringLiteral("\\Atssl-backup-[A-Za-z0-9_-]+\\.zip\\z"),
        QRegularExpression::CaseInsensitiveOption);
    return expression.match(name).hasMatch();
}

QString TsslBackupArchive::newFileName()
{
    return QStringLiteral("tssl-backup-%1-%2.zip")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz'Z'")),
            QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QStringList TsslBackupArchive::discover(const QString& directory)
{
    QStringList result;
    const auto files = QDir(directory).entryInfoList(QDir::Files | QDir::Readable | QDir::NoSymLinks, QDir::Name);
    for (const auto& file : files) {
        if (file.suffix().compare(QStringLiteral("tssl"), Qt::CaseInsensitive) == 0 || isArchiveName(file.fileName()))
            result.append(file.absoluteFilePath());
    }
    return result;
}

std::expected<int, QString> TsslBackupArchive::create(const QStringList& files, const QString& destination,
    const std::atomic_bool* canceled, const std::function<void(int, int)>& progress)
{
    if (isCanceled(canceled)) return std::unexpected(canceledError());
    if (files.isEmpty() || files.size() > maximumEntries)
        return std::unexpected(QStringLiteral("Backup must contain between 1 and 10000 TSSL packages"));
    if (QFileInfo::exists(destination)) return std::unexpected(QStringLiteral("Backup archive already exists"));
    QSaveFile output(destination);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly)) return std::unexpected(QStringLiteral("Cannot create backup archive: %1").arg(output.errorString()));
    output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    IoContext context {&output, canceled, maximumArchiveBytes};
    Zip zip;
    zip.archive.m_pWrite = writeCallback;
    zip.archive.m_pIO_opaque = &context;
    if (!mz_zip_writer_init(&zip.archive, 0)) return std::unexpected(zip.error(QStringLiteral("Cannot initialize ZIP writer"), canceled));
    QJsonArray packages;
    QSet<QString> names;
    qint64 expandedBytes = 0;
    if (progress) progress(0, static_cast<int>(files.size()));
    for (const auto& path : files) {
        if (isCanceled(canceled)) return std::unexpected(canceledError());
        const auto data = readPackage(path);
        if (!data) return std::unexpected(data.error());
        const auto package = TsslPackage::parse(*data);
        if (!package) return std::unexpected(QStringLiteral("Cannot back up invalid TSSL package: %1").arg(package.error()));
        const auto digest = QString::fromLatin1(package->rootManifestDigest.toHex());
        const auto name = digest + QStringLiteral(".tssl");
        if (names.contains(name)) return std::unexpected(QStringLiteral("Backup contains duplicate TSSL root digests"));
        names.insert(name);
        expandedBytes += data->size();
        if (expandedBytes > maximumExpandedBytes)
            return std::unexpected(QStringLiteral("Expanded backup exceeds the 2 GiB limit"));
        packages.append(QJsonObject {
            {QStringLiteral("file"), name},
            {QStringLiteral("rootManifestSha256"), digest},
            {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(*data, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("size"), QJsonValue::fromVariant(data->size())},
        });
        // One package is bounded by the existing TSSL parser. ZIP bytes are
        // compressed directly to the file callback; the archive is never held
        // in memory. Level 3 avoids spending CPU on marginal size savings.
        const auto encodedName = name.toLatin1();
        if (!mz_zip_writer_add_mem(&zip.archive, encodedName.constData(), data->constData(), data->size(), 3))
            return std::unexpected(zip.error(QStringLiteral("Cannot compress TSSL package"), canceled));
        if (progress) progress(static_cast<int>(packages.size()), static_cast<int>(files.size()));
    }
    const auto manifest = QJsonDocument(QJsonObject {
        {QStringLiteral("format"), QStringLiteral("TSSL-BACKUP")},
        {QStringLiteral("version"), 1},
        {QStringLiteral("count"), static_cast<int>(packages.size())},
        {QStringLiteral("createdAt"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("packages"), packages},
    }).toJson(QJsonDocument::Compact);
    if (manifest.size() > maximumManifestBytes) return std::unexpected(QStringLiteral("Backup manifest exceeds the 4 MiB limit"));
    if (expandedBytes + manifest.size() > maximumExpandedBytes)
        return std::unexpected(QStringLiteral("Expanded backup exceeds the 2 GiB limit"));
    if (!mz_zip_writer_add_mem(&zip.archive, "manifest.json", manifest.constData(), manifest.size(), 3)
        || !mz_zip_writer_finalize_archive(&zip.archive))
        return std::unexpected(zip.error(QStringLiteral("Cannot finalize backup archive"), canceled));
    mz_zip_writer_end(&zip.archive);
    if (isCanceled(canceled)) return std::unexpected(canceledError());
    if (QFileInfo::exists(destination)) return std::unexpected(QStringLiteral("Backup archive already exists"));
    if (!output.commit()) return std::unexpected(QStringLiteral("Cannot publish backup archive: %1").arg(output.errorString()));
    return static_cast<int>(packages.size());
}

std::expected<QStringList, QString> TsslBackupArchive::extract(const QString& archivePath, const QString& destinationDirectory,
    const std::atomic_bool* canceled, const std::function<void(int, int)>& progress)
{
    if (isCanceled(canceled)) return std::unexpected(canceledError());
    QFile input(archivePath);
    if (!input.open(QIODevice::ReadOnly)) return std::unexpected(QStringLiteral("Cannot read backup archive: %1").arg(input.errorString()));
    if (input.size() <= 0 || input.size() > maximumArchiveBytes)
        return std::unexpected(QStringLiteral("Backup archive is empty or exceeds the 2 GiB limit"));
    if (const auto validDirectory = checkDirectoryBounds(input); !validDirectory) return std::unexpected(validDirectory.error());
    IoContext context {&input, canceled, input.size()};
    Zip zip;
    zip.archive.m_pRead = readCallback;
    zip.archive.m_pIO_opaque = &context;
    if (!mz_zip_reader_init(&zip.archive, input.size(), 0)) return std::unexpected(zip.error(QStringLiteral("Cannot open ZIP archive"), canceled));
    const auto count = mz_zip_reader_get_num_files(&zip.archive);
    if (count < 2 || count > maximumEntries + 1) return std::unexpected(QStringLiteral("Backup entry count exceeds limits"));
    std::vector<Entry> entries;
    entries.reserve(count);
    QSet<QString> names;
    qint64 expandedBytes = 0;
    int manifestIndex = -1;
    for (mz_uint index = 0; index < count; ++index) {
        if (isCanceled(canceled)) return std::unexpected(canceledError());
        mz_zip_archive_file_stat stat {};
        if (!mz_zip_reader_file_stat(&zip.archive, index, &stat)) return std::unexpected(zip.error(QStringLiteral("Cannot read ZIP entry"), canceled));
        const auto name = QString::fromLatin1(stat.m_filename);
        const auto mode = (stat.m_external_attr >> 16) & 0170000;
        if ((name != manifestName && !isPackageName(name)) || names.contains(name)
            || stat.m_is_directory || stat.m_is_encrypted || !stat.m_is_supported
            || (mode != 0 && mode != 0100000) || (stat.m_external_attr & 0x10)
            || mz_zip_reader_get_filename(&zip.archive, index, nullptr, 0) != static_cast<mz_uint>(name.size() + 1)
            || stat.m_uncomp_size == 0
            || stat.m_uncomp_size > static_cast<mz_uint64>(name == manifestName ? maximumManifestBytes : maxPackageBytes))
            return std::unexpected(QStringLiteral("Backup contains an unsafe, duplicate, unsupported, or oversized entry"));
        names.insert(name);
        expandedBytes += static_cast<qint64>(stat.m_uncomp_size);
        if (expandedBytes > maximumExpandedBytes) return std::unexpected(QStringLiteral("Expanded backup exceeds the 2 GiB limit"));
        if (!mz_zip_validate_file(&zip.archive, index, MZ_ZIP_FLAG_VALIDATE_HEADERS_ONLY))
            return std::unexpected(zip.error(QStringLiteral("ZIP entry header mismatch"), canceled));
        entries.push_back({index, name, static_cast<qint64>(stat.m_uncomp_size)});
        if (name == manifestName) manifestIndex = static_cast<int>(index);
    }
    if (manifestIndex < 0) return std::unexpected(QStringLiteral("Backup manifest is missing"));
    const auto manifestStat = std::ranges::find_if(entries, [](const Entry& entry) { return entry.name == manifestName; });
    QByteArray manifestBytes(static_cast<qsizetype>(manifestStat->size), Qt::Uninitialized);
    if (!mz_zip_reader_extract_to_mem(&zip.archive, manifestIndex, manifestBytes.data(), manifestBytes.size(), 0))
        return std::unexpected(zip.error(QStringLiteral("Cannot read backup manifest"), canceled));
    const auto manifest = parseManifest(manifestBytes, static_cast<int>(count - 1));
    if (!manifest) return std::unexpected(manifest.error());
    const QFileInfo destinationInfo(destinationDirectory);
    if (destinationInfo.isSymLink() || (destinationInfo.exists() && !destinationInfo.isDir())
        || !QDir().mkpath(destinationDirectory))
        return std::unexpected(QStringLiteral("Restore destination must be a writable directory"));
    QTemporaryDir staging(QDir(destinationDirectory).filePath(QStringLiteral(".tssl-restore-XXXXXX")));
    if (!staging.isValid()) return std::unexpected(QStringLiteral("Cannot create temporary restore directory"));
    QStringList stagedNames;
    const auto total = static_cast<int>(count - 1);
    if (progress) progress(0, total);
    for (const auto& entry : entries) {
        if (entry.name == manifestName) continue;
        if (isCanceled(canceled)) return std::unexpected(canceledError());
        const auto expected = manifest->constFind(entry.name);
        if (expected == manifest->cend() || expected->size != entry.size)
            return std::unexpected(QStringLiteral("ZIP contents do not match the backup manifest"));
        QFile packageFile(staging.filePath(entry.name));
        if (!packageFile.open(QIODevice::WriteOnly)) return std::unexpected(QStringLiteral("Cannot write temporary TSSL package"));
        packageFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        IoContext extractedContext {&packageFile, canceled, maxPackageBytes};
        if (!mz_zip_reader_extract_to_callback(&zip.archive, entry.index, writeCallback, &extractedContext, 0))
            return std::unexpected(zip.error(QStringLiteral("Cannot decompress TSSL package"), canceled));
        packageFile.close();
        const auto bytes = readPackage(packageFile.fileName());
        if (!bytes) return std::unexpected(bytes.error());
        if (QString::fromLatin1(QCryptographicHash::hash(*bytes, QCryptographicHash::Sha256).toHex()) != expected->sha256)
            return std::unexpected(QStringLiteral("TSSL package SHA-256 does not match the backup manifest"));
        const auto package = TsslPackage::parse(*bytes);
        if (!package || QString::fromLatin1(package->rootManifestDigest.toHex()) != expected->digest)
            return std::unexpected(QStringLiteral("TSSL package is invalid or its root digest does not match the backup manifest"));
        stagedNames.append(entry.name);
        if (progress) progress(static_cast<int>(stagedNames.size()), total);
    }
    if (isCanceled(canceled)) return std::unexpected(canceledError());
    // Validate every entry before any package becomes visible to the caller.
    // Publishing never overwrites a destination file; roll back our new files
    // if a later rename fails or cancellation is requested.
    QStringList published;
    for (const auto& name : stagedNames) {
        const auto destination = QDir(destinationDirectory).filePath(name);
        if (isCanceled(canceled) || !QFile::rename(staging.filePath(name), destination)) {
            for (const auto& path : published) QFile::remove(path);
            return std::unexpected(isCanceled(canceled) ? canceledError() : QStringLiteral("Cannot publish restored TSSL package; destination may already exist"));
        }
        published.append(QFileInfo(destination).absoluteFilePath());
    }
    return published;
}
