#pragma once

#include <QByteArray>
#include <QHash>
#include <QString>

#include <expected>
#include <atomic>
#include <optional>
#include <vector>

struct EncryptedHlsTarEntry final {
    QString path;
    qint64 headerOffset { 0 };
    qint64 dataOffset { 0 };
    qint64 size { 0 };
    QByteArray sha256;
};

struct EncryptedHlsTarIndex final {
    int version { 1 };
    qint64 containerLength { 0 };
    QString manifestPath;
    std::vector<EncryptedHlsTarEntry> entries;
    QByteArray serialized;
    QByteArray sha256;

    const EncryptedHlsTarEntry* entry(const QString& path) const;
};

namespace EncryptedHlsTarContainer {

std::expected<EncryptedHlsTarIndex, QString> build(const QString& directoryPath,
                                                   const QString& outputPath,
                                                   const QString& manifestFileName,
                                                   std::atomic_bool* cancelRequested = nullptr);

std::expected<EncryptedHlsTarIndex, QString> readIndex(const QString& archivePath);
std::expected<EncryptedHlsTarIndex, QString> readIndexPrefix(QByteArrayView prefix,
                                                             qint64 containerLength);

std::expected<QByteArray, QString> readEntry(const QString& archivePath,
                                             const EncryptedHlsTarIndex& index,
                                             const QString& path,
                                             qint64 maximumBytes);

}
