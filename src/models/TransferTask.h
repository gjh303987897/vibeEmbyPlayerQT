#pragma once

#include <QString>

struct TransferTask {
    QString id;
    QString title;
    QString direction;
    QString status;
    QString detail;
    QString source;
    QString target;
    qint64 bytesDone { 0 };
    qint64 bytesTotal { -1 };
    qint64 bytesPerSecond { 0 };
    double progress { 0.0 };
    bool cancellable { true };
};
