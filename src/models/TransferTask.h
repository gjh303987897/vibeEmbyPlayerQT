#pragma once

#include <QString>

struct TransferTask {
    QString id;
    QString parentId;
    QString title;
    QString direction;
    QString status;
    QString detail;
    QString source;
    QString target;
    qint64 bytesDone { 0 };
    qint64 bytesTotal { -1 };
    qint64 bytesPerSecond { 0 };
    qint64 averageBytesPerSecond { 0 };
    qint64 bytesRemaining { -1 };
    double progress { 0.0 };
    int fileCount { 0 };
    int completedFileCount { 0 };
    bool isGroup { false };
    bool cancellable { true };
};
