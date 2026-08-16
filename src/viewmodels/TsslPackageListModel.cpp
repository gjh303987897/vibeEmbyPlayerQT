#include "viewmodels/TsslPackageListModel.h"

#include <QFileInfo>

#include <algorithm>

TsslPackageListModel::TsslPackageListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int TsslPackageListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_packages.size());
}

int TsslPackageListModel::count() const
{
    return rowCount();
}

int TsslPackageListModel::validCount() const
{
    return static_cast<int>(std::ranges::count(m_packages, true, &TsslPackageInfo::valid));
}

QVariant TsslPackageListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }
    const auto& package = m_packages.at(static_cast<size_t>(index.row()));
    switch (role) {
    case RootDigestRole:
        return QString::fromLatin1(package.rootManifestDigest.toHex());
    case IdentifierPreviewRole:
        return QStringLiteral("%1...%2")
            .arg(QString::fromLatin1(package.identifier.first(16)),
                 QString::fromLatin1(package.identifier.last(12)));
    case IdentifierLengthRole:
        return package.identifier.size();
    case FilePathRole:
        return package.filePath;
    case FileNameRole:
        return QFileInfo(package.filePath).fileName();
    case ModifiedAtRole:
        return package.modifiedAt;
    case FileSizeRole:
        return package.fileSize;
    case ManifestCountRole:
        return package.manifestCount;
    case SegmentCountRole:
        return package.segmentCount;
    case ResourceCountRole:
        return package.resourceCount;
    case ValidRole:
        return package.valid;
    case ValidationErrorRole:
        return package.validationError;
    default:
        return {};
    }
}

QHash<int, QByteArray> TsslPackageListModel::roleNames() const
{
    return {
        { RootDigestRole, "rootDigest" },
        { IdentifierPreviewRole, "identifierPreview" },
        { IdentifierLengthRole, "identifierLength" },
        { FilePathRole, "filePath" },
        { FileNameRole, "fileName" },
        { ModifiedAtRole, "modifiedAt" },
        { FileSizeRole, "fileSize" },
        { ManifestCountRole, "manifestCount" },
        { SegmentCountRole, "segmentCount" },
        { ResourceCountRole, "resourceCount" },
        { ValidRole, "validPackage" },
        { ValidationErrorRole, "validationError" },
    };
}

QVariantList TsslPackageListModel::validRows() const
{
    QVariantList rows;
    rows.reserve(validCount());
    for (int row = 0; row < rowCount(); ++row) {
        if (m_packages[static_cast<size_t>(row)].valid) {
            rows.push_back(row);
        }
    }
    return rows;
}

void TsslPackageListModel::setPackages(std::vector<TsslPackageInfo> packages)
{
    beginResetModel();
    m_packages = std::move(packages);
    endResetModel();
    emit countChanged();
}

std::optional<TsslPackageInfo> TsslPackageListModel::packageAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return m_packages.at(static_cast<size_t>(row));
}
