#pragma once

#include "services/webdav/TsslStore.h"

#include <QAbstractListModel>
#include <QStringList>
#include <QVariantList>

#include <optional>
#include <vector>

class TsslPackageListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countChanged)
    Q_PROPERTY(int validCount READ validCount NOTIFY countChanged)
    Q_PROPERTY(QString dateFilter READ dateFilter WRITE setDateFilter NOTIFY dateFilterChanged)
    Q_PROPERTY(QStringList availableDates READ availableDates NOTIFY availableDatesChanged)

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
    int totalCount() const;
    int validCount() const;
    QString dateFilter() const;
    void setDateFilter(const QString& value);
    QStringList availableDates() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    Q_INVOKABLE QVariantList validRows() const;
    Q_INVOKABLE QVariantList allRows() const;

    void setPackages(std::vector<TsslPackageInfo> packages);
    std::optional<TsslPackageInfo> packageAt(int row) const;

signals:
    void countChanged();
    void dateFilterChanged();
    void availableDatesChanged();

private:
    void rebuildFilteredPackages();

    std::vector<TsslPackageInfo> m_allPackages;
    std::vector<TsslPackageInfo> m_packages;
    QString m_dateFilter;
    QStringList m_availableDates;
};
