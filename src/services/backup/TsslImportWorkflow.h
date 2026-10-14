#pragma once
#include "services/webdav/TsslStore.h"
#include <QStringList>
#include <atomic>
#include <functional>

struct TsslImportSummary final {
    int restored { 0 };
    int alreadyExists { 0 };
    int failed { 0 };
    bool canceled { false };
    QString firstError;
};

namespace TsslImportWorkflow {
TsslImportSummary run(const TsslStore& store, const QStringList& paths,
                      const std::atomic_bool* canceled = nullptr,
                      const std::function<void(int, int)>& progress = {},
                      const std::function<void(const QString&, int, int)>& stageProgress = {});
}
