#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QDateTime>
#include <QHash>
#include <QString>

#include <expected>
#include <optional>
#include <span>
#include <vector>

struct TsslPackage final {
    static constexpr qsizetype identifierLength = 4096;

    int version { 2 };
    QString containerFormat;
    QByteArray containerIndexSha256;
    qint64 containerLength { 0 };
    QByteArray identifier;
    QByteArray rootManifestDigest;
    QByteArray encryptedSourceFileName;
    QByteArray sourceFileNameKey;
    QHash<QString, QByteArray> manifestDigests;
    QHash<QString, QByteArray> segmentKeys;
    QHash<QString, QByteArray> resourceDigests;

    static std::expected<TsslPackage, QString> parse(QByteArrayView document);
    static QByteArray sourceFileNameAuthenticatedData(QByteArrayView identifier);
    QByteArray toJson() const;
    std::expected<std::optional<QString>, QString> decryptedSourceFileName() const;
};

struct TsslPackageInfo final {
    QByteArray identifier;
    QByteArray rootManifestDigest;
    QString filePath;
    QDateTime modifiedAt;
    qint64 fileSize { 0 };
    int manifestCount { 0 };
    int segmentCount { 0 };
    int resourceCount { 0 };
    bool valid { true };
    QString validationError;
};

// Cheap catalogue entry: file name, size and timestamp only. Listing a storage
// directory full of packages must not read or parse any of them, so the browser
// can show totals, dates and pages without paying for a full scan. Deciding
// whether a package is valid requires parsing, so a summary never claims more
// than "this looks like a package file".
struct TsslPackageSummary final {
    QByteArray rootManifestDigest;
    QString filePath;
    QDateTime modifiedAt;
    qint64 fileSize { 0 };
};

class TsslStore final {
public:
    explicit TsslStore(QString storageDirectory = {});

    static QString packageAlreadyExistsError();
    std::expected<void, QString> savePackage(const TsslPackage& package) const;
    std::expected<std::optional<TsslPackage>, QString> packageForRootDigest(QByteArrayView digest) const;
    std::expected<std::vector<TsslPackageInfo>, QString> listPackages() const;
    // Enumerates package files without opening any of them.
    std::expected<std::vector<TsslPackageSummary>, QString> listPackageSummaries() const;
    // Parses exactly the listed files, which must come from this store.
    std::expected<std::vector<TsslPackageInfo>, QString> packageInfosForPaths(const QStringList& paths) const;
    std::expected<QByteArray, QString> restoreFromFile(const QString& sourcePath) const;
    std::expected<void, QString> exportByRootDigest(QByteArrayView digest, const QString& destinationPath) const;
    std::expected<int, QString> exportByRootDigests(std::span<const QByteArray> digests,
                                                    const QString& destinationDirectory) const;
    std::expected<void, QString> deleteByRootDigest(QByteArrayView digest) const;
    QString storageDirectory() const;

private:
    std::expected<void, QString> ensureStorageDirectory() const;
    QString packagePath(QByteArrayView digest) const;

    QString m_storageDirectory;
};
