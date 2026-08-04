#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QDateTime>
#include <QHash>
#include <QString>

#include <expected>
#include <optional>
#include <vector>

struct TsslPackage final {
    static constexpr qsizetype identifierLength = 4096;

    QByteArray identifier;
    QByteArray rootManifestDigest;
    QHash<QString, QByteArray> manifestDigests;
    QHash<QString, QByteArray> segmentKeys;
    QHash<QString, QByteArray> resourceDigests;

    static std::expected<TsslPackage, QString> parse(QByteArrayView document);
    QByteArray toJson() const;
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

class TsslStore final {
public:
    explicit TsslStore(QString storageDirectory = {});

    std::expected<void, QString> savePackage(const TsslPackage& package) const;
    std::expected<std::optional<TsslPackage>, QString> packageForRootDigest(QByteArrayView digest) const;
    std::expected<std::vector<TsslPackageInfo>, QString> listPackages() const;
    std::expected<QByteArray, QString> restoreFromFile(const QString& sourcePath) const;
    std::expected<void, QString> exportByRootDigest(QByteArrayView digest, const QString& destinationPath) const;
    std::expected<void, QString> deleteByRootDigest(QByteArrayView digest) const;
    QString storageDirectory() const;

private:
    std::expected<void, QString> ensureStorageDirectory() const;
    QString packagePath(QByteArrayView digest) const;

    QString m_storageDirectory;
};
