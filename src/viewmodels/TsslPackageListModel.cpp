#include "viewmodels/TsslPackageListModel.h"

#include <QFileInfo>

#include <algorithm>
#include <functional>

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

int TsslPackageListModel::totalCount() const
{
    // With a catalogue the count describes every package file on disk, not just the
    // page that has been parsed so far.
    return static_cast<int>(m_summaries.empty() ? m_allPackages.size() : m_summaries.size());
}

int TsslPackageListModel::validCount() const
{
    return static_cast<int>(std::ranges::count(m_packages, true, &TsslPackageInfo::valid));
}

QString TsslPackageListModel::dateFilter() const
{
    return m_dateFilter;
}

void TsslPackageListModel::setDateFilter(const QString& value)
{
    const auto normalized = m_availableDates.contains(value) ? value : QString {};
    if (m_dateFilter == normalized) {
        return;
    }
    m_dateFilter = normalized;
    rebuildFilteredPackages();
    emit dateFilterChanged();
}

void TsslPackageListModel::setSummaries(std::vector<TsslPackageSummary> summaries)
{
    m_summaries = std::move(summaries);
    // The catalogue replaces the previous listing, so anything parsed from it is now
    // stale; the view model refills the first page straight afterwards.
    refreshCatalogueDates();
    if (!m_dateFilter.isEmpty() && !m_availableDates.contains(m_dateFilter)) {
        m_dateFilter.clear();
        emit dateFilterChanged();
    }
    setPackages({});
}

const std::vector<TsslPackageSummary>& TsslPackageListModel::summaries() const
{
    return m_summaries;
}

QStringList TsslPackageListModel::filteredSummaryPaths() const
{
    if (m_summaries.empty()) {
        return {};
    }
    QStringList paths;
    paths.reserve(static_cast<qsizetype>(m_summaries.size()));
    for (const auto& summary : m_summaries) {
        if (m_dateFilter.isEmpty()
            || (summary.modifiedAt.isValid()
                && summary.modifiedAt.date().toString(Qt::ISODate) == m_dateFilter)) {
            paths.append(summary.filePath);
        }
    }
    return paths;
}

QStringList TsslPackageListModel::availableDates() const
{
    return m_availableDates;
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

QVariantList TsslPackageListModel::allRows() const
{
    QVariantList rows;
    rows.reserve(rowCount());
    for (int row = 0; row < rowCount(); ++row) {
        rows.push_back(row);
    }
    return rows;
}

void TsslPackageListModel::setPackages(std::vector<TsslPackageInfo> packages)
{
    m_allPackages = std::move(packages);
    if (m_summaries.empty()) {
        // Full-list mode: the parsed packages are the only thing known about the
        // store, so they also drive the total and the date filter options.
        QStringList dates;
        dates.reserve(static_cast<qsizetype>(m_allPackages.size()));
        for (const auto& package : m_allPackages) {
            if (package.modifiedAt.isValid()) {
                dates.push_back(package.modifiedAt.date().toString(Qt::ISODate));
            }
        }
        dates.removeDuplicates();
        std::ranges::sort(dates, std::greater {});
        if (m_availableDates != dates) {
            m_availableDates = std::move(dates);
            emit availableDatesChanged();
        }
        if (!m_dateFilter.isEmpty() && !m_availableDates.contains(m_dateFilter)) {
            m_dateFilter.clear();
            emit dateFilterChanged();
        }
    }
    rebuildFilteredPackages();
}

void TsslPackageListModel::appendPackages(std::vector<TsslPackageInfo> packages)
{
    if (packages.empty()) {
        return;
    }
    // Decide visibility before announcing rows, so the view is never told about a
    // package the date filter hides.
    std::vector<TsslPackageInfo> visible;
    visible.reserve(packages.size());
    for (const auto& package : packages) {
        if (m_dateFilter.isEmpty()
            || (package.modifiedAt.isValid()
                && package.modifiedAt.date().toString(Qt::ISODate) == m_dateFilter)) {
            visible.push_back(package);
        }
    }
    const auto firstRow = static_cast<int>(m_packages.size());
    if (!visible.empty()) {
        beginInsertRows({}, firstRow, firstRow + static_cast<int>(visible.size()) - 1);
    }
    m_allPackages.insert(m_allPackages.end(),
                         std::make_move_iterator(packages.begin()),
                         std::make_move_iterator(packages.end()));
    m_packages.insert(m_packages.end(),
                      std::make_move_iterator(visible.begin()),
                      std::make_move_iterator(visible.end()));
    if (!visible.empty()) {
        endInsertRows();
    }
    emit countChanged();
}

void TsslPackageListModel::refreshCatalogueDates()
{
    QStringList dates;
    dates.reserve(static_cast<qsizetype>(m_summaries.size()));
    for (const auto& summary : m_summaries) {
        if (summary.modifiedAt.isValid()) {
            dates.push_back(summary.modifiedAt.date().toString(Qt::ISODate));
        }
    }
    dates.removeDuplicates();
    std::ranges::sort(dates, std::greater {});
    if (m_availableDates != dates) {
        m_availableDates = std::move(dates);
        emit availableDatesChanged();
    }
}

std::optional<TsslPackageInfo> TsslPackageListModel::packageAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return std::nullopt;
    }
    return m_packages.at(static_cast<size_t>(row));
}

void TsslPackageListModel::rebuildFilteredPackages()
{
    beginResetModel();
    m_packages.clear();
    m_packages.reserve(m_allPackages.size());
    for (const auto& package : m_allPackages) {
        if (m_dateFilter.isEmpty()
            || (package.modifiedAt.isValid()
                && package.modifiedAt.date().toString(Qt::ISODate) == m_dateFilter)) {
            m_packages.push_back(package);
        }
    }
    endResetModel();
    emit countChanged();
}
