#pragma once

#include "services/webdav/TsslStore.h"

#include <QAbstractListModel>
#include <QVariantList>

#include <optional>
#include <vector>

class TsslPackageListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int validCount READ validCount NOTIFY countChanged)

public:
    enum Role {
        RootDigestRole = Qt::UserRole + 1,
        IdentifierPreviewRole,
        IdentifierLengthRole,
        FilePathRole,
        FileNameRole,
        ModifiedAtRole,
        FileSizeRole,
        ManifestCountRole,
        SegmentCountRole,
        ResourceCountRole,
        ValidRole,
        ValidationErrorRole,
    };

    explicit TsslPackageListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = {}) const override;
    int count() const;
    int validCount() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE QVariantList validRows() const;

    void setPackages(std::vector<TsslPackageInfo> packages);
    std::optional<TsslPackageInfo> packageAt(int row) const;

signals:
    void countChanged();

private:
    std::vector<TsslPackageInfo> m_packages;
};
