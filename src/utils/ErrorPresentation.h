#pragma once

#include <QString>

enum class AppErrorCode {
    Unknown,
    Validation,
    Network,
    Authentication,
    File,
    Playback,
    Packaging,
    Storage,
    Update,
};

struct AppErrorPresentation {
    AppErrorCode code { AppErrorCode::Unknown };
    QString titleKey;
    QString summaryKey;
    QString hintKey;
    QString details;
};

class ErrorPresentation final
{
public:
    static AppErrorPresentation fromMessage(const QString& message);
    static QString sanitizeDetails(QString details);
};
