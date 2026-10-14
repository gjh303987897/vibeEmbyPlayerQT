#include "services/backup/TsslImportWorkflow.h"
#include "services/backup/TsslBackupArchive.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

TsslImportSummary TsslImportWorkflow::run(const TsslStore& store, const QStringList& paths,
                                         const std::atomic_bool* canceled,
                                         const std::function<void(int, int)>& progress,
                                         const std::function<void(const QString&, int, int)>& stageProgress)
{
    TsslImportSummary summary;
    QTemporaryDir extracted;
    QStringList packages;
    int archiveIndex = 0;
    for (const auto& path : paths) {
        if (canceled && canceled->load()) { summary.canceled = true; return summary; }
        if (QFileInfo(path).suffix().compare(QStringLiteral("zip"), Qt::CaseInsensitive) != 0) {
            packages.append(path);
            continue;
        }
        const auto result = extracted.isValid()
            ? TsslBackupArchive::extract(path, QDir(extracted.path()).filePath(QString::number(archiveIndex++)),
                canceled, [&](int done, int total) {
                    if (stageProgress) stageProgress(QStringLiteral("extracting"), done, total);
                })
            : std::expected<QStringList, QString>(std::unexpected(QStringLiteral("Unable to create a TSSL extraction directory")));
        if (canceled && canceled->load()) { summary.canceled = true; return summary; }
        if (result) packages.append(*result);
        else {
            ++summary.failed;
            if (summary.firstError.isEmpty()) summary.firstError = result.error();
        }
    }
    int processed = 0;
    if (stageProgress) stageProgress(QStringLiteral("importing"), 0, packages.size());
    for (const auto& path : packages) {
        if (canceled && canceled->load()) { summary.canceled = true; break; }
        const auto restored = store.restoreFromFile(path);
        if (restored) ++summary.restored;
        else if (restored.error() == TsslStore::packageAlreadyExistsError()) ++summary.alreadyExists;
        else {
            ++summary.failed;
            if (summary.firstError.isEmpty()) summary.firstError = restored.error();
        }
        ++processed;
        if (progress) progress(processed, packages.size());
        if (stageProgress) stageProgress(QStringLiteral("importing"), processed, packages.size());
    }
    return summary;
}
