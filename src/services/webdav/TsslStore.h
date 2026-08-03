#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QHash>
#include <QString>

#include <expected>
#include <optional>

struct TsslPackage final {
    QByteArray rootManifestDigest;
    QHash<QString, QByteArray> manifestDigests;
    QHash<QString, QByteArray> segmentKeys;
    QHash<QString, QByteArray> resourceDigests;

    static std::expected<TsslPackage, QString> parse(QByteArrayView document);
    QByteArray toJson() const;
};

class TsslStore final {
public:
    explicit TsslStore(QString storageDirectory = {});

    std::expected<std::optional<TsslPackage>, QString> packageForRootDigest(QByteArrayView digest) const;
    std::expected<QByteArray, QString> restoreFromFile(const QString& sourcePath) const;
    std::expected<void, QString> exportByRootDigest(QByteArrayView digest, const QString& destinationPath) const;
    QString storageDirectory() const;

private:
    std::expected<void, QString> ensureStorageDirectory() const;
    QString packagePath(QByteArrayView digest) const;

    QString m_storageDirectory;
};
