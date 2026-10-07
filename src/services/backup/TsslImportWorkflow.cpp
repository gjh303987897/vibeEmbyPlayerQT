#include "services/backup/TsslImportWorkflow.h"

TsslImportSummary TsslImportWorkflow::run(const TsslStore& store, const QStringList& paths,
                                         const std::atomic_bool* canceled,
                                         const std::function<void(int, int)>& progress)
{
    TsslImportSummary summary;
    int processed = 0;
    for (const auto& path : paths) {
        if (canceled && canceled->load()) { summary.canceled = true; break; }
        const auto restored = store.restoreFromFile(path);
        if (restored) ++summary.restored;
        else if (restored.error() == TsslStore::packageAlreadyExistsError()) ++summary.alreadyExists;
        else {
            ++summary.failed;
            if (summary.firstError.isEmpty()) summary.firstError = restored.error();
        }
        if (progress) progress(++processed, paths.size());
    }
    return summary;
}
