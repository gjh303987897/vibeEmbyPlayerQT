#include "services/encryptedhls/EncryptedHlsTarContainer.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QCborArray>
#include <QCborMap>
#include <QCborValue>

#include <algorithm>
#include <atomic>
#include <limits>

namespace {
constexpr qint64 blockSize = 512;
constexpr qint64 maximumIndexBytes = 16 * 1024 * 1024;
constexpr qint64 maximumEntries = 1'000'000;
constexpr qint64 maximumMemberBytes = 8LL * 1024 * 1024 * 1024;
constexpr qint64 maximumContainerBytes = 64LL * 1024 * 1024 * 1024;

qint64 aligned(qint64 value)
{
    return (value + blockSize - 1) / blockSize * blockSize;
}

QByteArray tarChecksumField(qint64 checksum)
{
    const auto digits = QByteArray::number(checksum, 8);
    if (digits.size() > 6) {
        return {};
    }
    // POSIX ustar stores six octal digits followed by NUL and space. Using
    // seven digits plus NUL is accepted by our parser but rejected by bsdtar.
    return QByteArray(6 - digits.size(), '0') + digits + '\0' + ' ';
}

std::expected<QString, QString> safePath(const QString& path)
{
    if (path.isEmpty() || path.contains(QLatin1Char('\\')) || path.startsWith(QLatin1Char('/')) ||
        (path.size() >= 2 && path.at(1) == QLatin1Char(':')) ||
        QDir::isAbsolutePath(path)) {
        return std::unexpected(QStringLiteral("TAR entry path is not relative"));
    }
    const auto clean = QDir::cleanPath(path);
    if (clean != path || clean == QStringLiteral(".") || clean == QStringLiteral("..") ||
        clean.startsWith(QStringLiteral("../"))) {
        return std::unexpected(QStringLiteral("TAR entry path escapes the package root"));
    }
    return clean;
}

QByteArray field(const QByteArray& value, qsizetype size)
{
    QByteArray result(size, '\0');
    result.replace(0, std::min(size, value.size()), value.left(size));
    return result;
}

QByteArray octal(qint64 value, qsizetype size)
{
    auto text = QByteArray::number(value, 8);
    if (text.size() + 1 > size) {
        return {};
    }
    QByteArray result(size, '0');
    const auto start = size - text.size() - 1;
    result.replace(start, text.size(), text);
    result[size - 1] = '\0';
    return result;
}

QByteArray tarHeader(const QString& path, qint64 size)
{
    const auto encoded = path.toUtf8();
    if (encoded.size() > 100 || size < 0) {
        return {};
    }
    QByteArray header(blockSize, '\0');
    header.replace(0, encoded.size(), encoded);
    header.replace(100, 8, field("0000644\0", 8));
    header.replace(108, 8, field("0000000\0", 8));
    header.replace(116, 8, field("0000000\0", 8));
    const auto sizeField = octal(size, 12);
    if (sizeField.isEmpty()) return {};
    header.replace(124, 12, sizeField);
    header.replace(136, 12, field("00000000000\0", 12));
    header.replace(148, 8, QByteArray(8, ' '));
    header[156] = '0';
    header.replace(257, 6, QByteArrayLiteral("ustar\0"));
    header.replace(263, 2, QByteArrayLiteral("00"));
    qint64 checksum = 0;
    for (const auto byte : header) checksum += static_cast<unsigned char>(byte);
    const auto checksumField = tarChecksumField(checksum);
    if (checksumField.isEmpty()) return {};
    header.replace(148, 8, checksumField);
    return header;
}

QByteArray tarHeader(const QString& path, qint64 size, char typeFlag)
{
    auto header = tarHeader(path, size);
    if (!header.isEmpty()) {
        header[156] = typeFlag;
        header.replace(148, 8, QByteArray(8, ' '));
        qint64 checksum = 0;
        for (const auto byte : header) checksum += static_cast<unsigned char>(byte);
        const auto checksumField = tarChecksumField(checksum);
        if (checksumField.isEmpty()) return {};
        header.replace(148, 8, checksumField);
    }
    return header;
}

QByteArray paxPathRecord(const QString& path)
{
    const auto value = QByteArrayLiteral("path=") + path.toUtf8() + '\n';
    auto length = value.size() + 2;
    while (true) {
        const auto digits = QByteArray::number(length).size();
        const auto actual = digits + 1 + value.size();
        if (actual == length) break;
        length = actual;
    }
    return QByteArray::number(length) + ' ' + value;
}

qint64 paxPrefixSize(const QString& path)
{
    if (path.toUtf8().size() <= 100) return 0;
    return blockSize + aligned(paxPathRecord(path).size());
}

bool validChecksum(const QByteArray& header)
{
    if (header.size() != blockSize) return false;
    bool ok = false;
    const auto expected = QByteArrayView(header).sliced(148, 8).trimmed().toLongLong(&ok, 8);
    if (!ok) return false;
    qint64 actual = 0;
    for (qsizetype i = 0; i < header.size(); ++i) {
        actual += static_cast<unsigned char>(i >= 148 && i < 156 ? ' ' : header.at(i));
    }
    return expected == actual;
}

std::expected<qint64, QString> parseOctalField(QByteArrayView fieldBytes)
{
    QByteArray text;
    bool started = false;
    for (const auto byte : fieldBytes) {
        if (!started && (byte == '\0' || byte == ' ')) continue;
        if (byte < '0' || byte > '7') break;
        started = true;
        text.append(byte);
    }
    bool ok = false;
    const auto value = text.toLongLong(&ok, 8);
    return ok && value >= 0 ? std::expected<qint64, QString>(value)
                            : std::unexpected(QStringLiteral("Invalid TAR numeric field"));
}

std::expected<QByteArray, QString> encodeIndex(const EncryptedHlsTarIndex& index)
{
    QCborMap root;
    root.insert(QStringLiteral("version"), index.version);
    root.insert(QStringLiteral("containerLength"), index.containerLength);
    root.insert(QStringLiteral("manifestPath"), index.manifestPath);
    QCborArray entries;
    for (const auto& entry : index.entries) {
        QCborMap value;
        value.insert(QStringLiteral("path"), entry.path);
        value.insert(QStringLiteral("headerOffset"), entry.headerOffset);
        value.insert(QStringLiteral("dataOffset"), entry.dataOffset);
        value.insert(QStringLiteral("size"), entry.size);
        value.insert(QStringLiteral("sha256"), entry.sha256.toHex());
        entries.append(value);
    }
    root.insert(QStringLiteral("entries"), entries);
    const auto bytes = QCborValue(root).toCbor();
    if (bytes.isEmpty() || bytes.size() > maximumIndexBytes) {
        return std::unexpected(QStringLiteral("TAR index is empty or too large"));
    }
    return bytes;
}

std::expected<EncryptedHlsTarIndex, QString> decodeIndex(QByteArrayView bytes)
{
    QCborParserError error;
    const auto value = QCborValue::fromCbor(QByteArray(bytes.data(), bytes.size()), &error);
    if (error.error != QCborError::NoError || !value.isMap()) {
        return std::unexpected(QStringLiteral("Invalid M3U8SP CBOR index"));
    }
    const auto root = value.toMap();
    const auto version = root.value(QStringLiteral("version"));
    if (!version.isInteger() || version.toInteger() != 1) {
        return std::unexpected(QStringLiteral("Unsupported M3U8SP index version"));
    }
    EncryptedHlsTarIndex result;
    result.version = 1;
    result.containerLength = root.value(QStringLiteral("containerLength")).toInteger();
    if (result.containerLength <= 0 || result.containerLength > maximumContainerBytes) {
        return std::unexpected(QStringLiteral("Invalid M3U8SP container length"));
    }
    result.manifestPath = root.value(QStringLiteral("manifestPath")).toString();
    auto manifest = safePath(result.manifestPath);
    if (!manifest || !result.manifestPath.endsWith(QStringLiteral(".m3u8s"), Qt::CaseInsensitive)) {
        return std::unexpected(QStringLiteral("Invalid M3U8SP manifest path"));
    }
    const auto array = root.value(QStringLiteral("entries"));
    if (!array.isArray() || array.toArray().size() <= 0 || array.toArray().size() > maximumEntries) {
        return std::unexpected(QStringLiteral("Invalid M3U8SP index entries"));
    }
    QHash<QString, bool> seen;
    for (const auto item : array.toArray()) {
        if (!item.isMap()) return std::unexpected(QStringLiteral("Invalid M3U8SP index entry"));
        const auto map = item.toMap();
        EncryptedHlsTarEntry entry;
        entry.path = map.value(QStringLiteral("path")).toString();
        auto path = safePath(entry.path);
        entry.headerOffset = map.value(QStringLiteral("headerOffset")).toInteger();
        entry.dataOffset = map.value(QStringLiteral("dataOffset")).toInteger();
        entry.size = map.value(QStringLiteral("size")).toInteger();
        entry.sha256 = QByteArray::fromHex(map.value(QStringLiteral("sha256")).toByteArray());
        if (!path || seen.contains(entry.path) || entry.headerOffset < 0 || entry.dataOffset < 512 ||
            entry.size < 0 || entry.size > maximumMemberBytes || entry.sha256.size() != 32 || entry.dataOffset != entry.headerOffset + 512) {
            return std::unexpected(QStringLiteral("Invalid or duplicate M3U8SP index entry"));
        }
        seen.insert(entry.path, true);
        result.entries.push_back(std::move(entry));
    }
    result.serialized = QByteArray(bytes.data(), bytes.size());
    result.sha256 = QCryptographicHash::hash(result.serialized, QCryptographicHash::Sha256);
    return result;
}
}

const EncryptedHlsTarEntry* EncryptedHlsTarIndex::entry(const QString& path) const
{
    const auto found = std::ranges::find_if(entries, [&path](const auto& value) { return value.path == path; });
    return found == entries.end() ? nullptr : &*found;
}

namespace EncryptedHlsTarContainer {
std::expected<EncryptedHlsTarIndex, QString> build(const QString& directoryPath,
                                                   const QString& outputPath,
                                                   const QString& manifestFileName,
                                                   std::atomic_bool* cancelRequested)
{
    const QFileInfo rootInfo(directoryPath);
    if (!rootInfo.isDir()) return std::unexpected(QStringLiteral("Encrypted HLS directory is unavailable"));
    QList<QFileInfo> files;
    QDirIterator iterator(directoryPath, QDir::Files | QDir::Readable, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        if (cancelRequested && cancelRequested->load()) return std::unexpected(QStringLiteral("Packaging canceled"));
        files.push_back(iterator.fileInfo());
    }
    std::ranges::sort(files, [](const auto& a, const auto& b) { return a.filePath() < b.filePath(); });
    EncryptedHlsTarIndex index;
    index.manifestPath = manifestFileName;
    for (const auto& file : files) {
        const auto relative = QDir(directoryPath).relativeFilePath(file.filePath()).replace('\\', '/');
        auto path = safePath(relative);
        if (!path || relative == QStringLiteral("index.m3u8")) continue;
        EncryptedHlsTarEntry entry;
        entry.path = *path;
        entry.size = file.size();
        QFile source(file.filePath());
        if (!source.open(QIODevice::ReadOnly)) return std::unexpected(QStringLiteral("Unable to read TAR source: %1").arg(relative));
        QCryptographicHash digest(QCryptographicHash::Sha256);
        while (!source.atEnd()) {
            const auto chunk = source.read(1024 * 1024);
            if (chunk.isEmpty() && source.error() != QFileDevice::NoError) return std::unexpected(QStringLiteral("Unable to hash TAR source: %1").arg(relative));
            digest.addData(chunk);
        }
        entry.sha256 = digest.result();
        index.entries.push_back(std::move(entry));
    }
    std::expected<QByteArray, QString> indexBytes;
    for (int pass = 0; pass < 4; ++pass) {
        indexBytes = encodeIndex(index);
        if (!indexBytes) return std::unexpected(indexBytes.error());
        qint64 offset = blockSize + aligned(indexBytes->size());
        for (auto& entry : index.entries) {
            offset += paxPrefixSize(entry.path);
            entry.headerOffset = offset;
            entry.dataOffset = offset + blockSize;
            offset += blockSize + aligned(entry.size);
        }
        index.containerLength = offset + blockSize * 2;
        if (index.containerLength > maximumContainerBytes) {
            return std::unexpected(QStringLiteral("M3U8SP container is too large"));
        }
    }
    indexBytes = encodeIndex(index);
    if (!indexBytes) return std::unexpected(indexBytes.error());
    index.serialized = *indexBytes;
    index.sha256 = QCryptographicHash::hash(index.serialized, QCryptographicHash::Sha256);
    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly)) return std::unexpected(QStringLiteral("Unable to create TAR container: %1").arg(output.errorString()));
    auto writeZeros = [&output](qint64 count) { return output.write(QByteArray(static_cast<qsizetype>(count), '\0')) == count; };
    const auto indexHeader = tarHeader(QStringLiteral(".vibe/index.cbor"), index.serialized.size());
    if (indexHeader.isEmpty() || output.write(indexHeader) != indexHeader.size() || output.write(index.serialized) != index.serialized.size() || !writeZeros(aligned(index.serialized.size()) - index.serialized.size())) {
        return std::unexpected(QStringLiteral("Unable to write TAR index"));
    }
    for (const auto& entry : index.entries) {
        if (cancelRequested && cancelRequested->load()) return std::unexpected(QStringLiteral("Packaging canceled"));
        const auto sourcePath = QDir(directoryPath).filePath(entry.path);
        QFile source(sourcePath);
        if (!source.open(QIODevice::ReadOnly)) return std::unexpected(QStringLiteral("Unable to read TAR source: %1").arg(entry.path));
        const auto pax = paxPathRecord(entry.path);
        if (entry.path.toUtf8().size() > 100) {
            const auto paxHeader = tarHeader(QStringLiteral(".vibe/pax/%1").arg(entry.headerOffset), pax.size(), 'x');
            if (paxHeader.isEmpty() || output.write(paxHeader) != paxHeader.size() ||
                output.write(pax) != pax.size() || !writeZeros(aligned(pax.size()) - pax.size())) {
                return std::unexpected(QStringLiteral("Unable to write TAR PAX header"));
            }
        }
        const auto header = tarHeader(entry.path.toUtf8().size() > 100
                                          ? QStringLiteral(".vibe/file/%1").arg(entry.headerOffset)
                                          : entry.path,
                                      entry.size);
        if (header.isEmpty() || output.write(header) != header.size()) return std::unexpected(QStringLiteral("Unable to write TAR header"));
        while (!source.atEnd()) {
            const auto chunk = source.read(1024 * 1024);
            if (chunk.isEmpty() && source.error() != QFileDevice::NoError) return std::unexpected(QStringLiteral("Unable to read TAR source"));
            if (output.write(chunk) != chunk.size()) return std::unexpected(QStringLiteral("Unable to write TAR content"));
        }
        if (!writeZeros(aligned(entry.size) - entry.size)) return std::unexpected(QStringLiteral("Unable to align TAR content"));
    }
    if (!writeZeros(blockSize * 2) || !output.commit()) return std::unexpected(QStringLiteral("Unable to commit TAR container"));
    return index;
}

std::expected<EncryptedHlsTarIndex, QString> readIndex(const QString& archivePath)
{
    QFile archive(archivePath);
    if (!archive.open(QIODevice::ReadOnly) || archive.size() < blockSize) return std::unexpected(QStringLiteral("Unable to open M3U8SP container"));
    const auto prefix = archive.read(std::min<qint64>(archive.size(), maximumIndexBytes + blockSize));
    auto index = readIndexPrefix(prefix, archive.size());
    if (!index) return index;
    for (const auto& entry : index->entries) {
        if (!archive.seek(entry.headerOffset)) {
            return std::unexpected(QStringLiteral("Unable to seek M3U8SP TAR entry"));
        }
        const auto entryHeader = archive.read(blockSize);
        if (entryHeader.size() != blockSize || entryHeader.at(156) != '0' || !validChecksum(entryHeader)) {
            return std::unexpected(QStringLiteral("Invalid M3U8SP TAR entry header"));
        }
        const auto headerSize = parseOctalField(QByteArrayView(entryHeader).sliced(124, 12));
        if (!headerSize || *headerSize != entry.size) {
            return std::unexpected(QStringLiteral("M3U8SP TAR entry size does not match index"));
        }
    }
    return index;
}

std::expected<EncryptedHlsTarIndex, QString> readIndexPrefix(QByteArrayView prefix,
                                                             qint64 containerLength)
{
    if (prefix.size() < blockSize || containerLength < blockSize * 3) {
        return std::unexpected(QStringLiteral("Incomplete M3U8SP TAR header"));
    }
    const auto header = QByteArray(prefix.data(), blockSize);
    if (header.size() != blockSize || QByteArrayView(header).sliced(257, 5) != QByteArrayView("ustar", 5) ||
        QByteArrayView(header).sliced(0, 17) != QByteArrayView(".vibe/index.cbor", 17) ||
        header.at(156) != '0' || !validChecksum(header)) {
        return std::unexpected(QStringLiteral("Invalid M3U8SP TAR header"));
    }
    const auto size = parseOctalField(QByteArrayView(header).sliced(124, 12));
    if (!size || *size <= 0 || *size > maximumIndexBytes) return std::unexpected(QStringLiteral("Invalid M3U8SP index size"));
    if (*size > prefix.size() - blockSize) return std::unexpected(QStringLiteral("Incomplete M3U8SP index"));
    const auto bytes = QByteArray(prefix.data() + blockSize, *size);
    auto index = decodeIndex(bytes);
    if (!index) return index;
    if (index->containerLength != containerLength) {
        return std::unexpected(QStringLiteral("M3U8SP container length does not match TAR"));
    }
    qint64 previousEnd = blockSize + aligned(*size);
    for (const auto& entry : index->entries) {
        if (entry.headerOffset < previousEnd || entry.dataOffset < entry.headerOffset ||
            entry.size > index->containerLength - entry.dataOffset ||
            entry.dataOffset + aligned(entry.size) > index->containerLength - blockSize * 2) {
            return std::unexpected(QStringLiteral("M3U8SP index entry is outside the container"));
        }
        previousEnd = entry.dataOffset + aligned(entry.size);
    }
    return index;
}

std::expected<QByteArray, QString> readEntry(const QString& archivePath,
                                             const EncryptedHlsTarIndex& index,
                                             const QString& path,
                                             qint64 maximumBytes)
{
    const auto* entry = index.entry(path);
    if (!entry || maximumBytes < 0 || entry->size > maximumBytes) return std::unexpected(QStringLiteral("M3U8SP entry is unavailable or too large"));
    QFile archive(archivePath);
    if (!archive.open(QIODevice::ReadOnly) || !archive.seek(entry->dataOffset)) return std::unexpected(QStringLiteral("Unable to read M3U8SP entry"));
    const auto bytes = archive.read(entry->size);
    if (bytes.size() != entry->size || QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != entry->sha256) return std::unexpected(QStringLiteral("M3U8SP entry digest mismatch"));
    return bytes;
}
}
