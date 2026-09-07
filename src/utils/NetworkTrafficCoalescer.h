#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>

#include <QString>

// Batches per-socket/per-reply network traffic counters into one sample per
// interval. Download loops fire readyRead dozens of times per second; emitting
// a UI-facing signal on every chunk floods the event loop and the usage
// bookkeeping downstream. Callers feed raw byte counts here and forward the
// coalesced `flushed` signal instead. See VIBEDOCS/NetworkTrafficCoalescer.md.
class NetworkTrafficCoalescer final : public QObject {
    Q_OBJECT

public:
    explicit NetworkTrafficCoalescer(QObject* parent = nullptr);

    void record(const QString& serviceId,
                const QString& serviceName,
                const QString& serviceType,
                qint64 bytesReceived,
                qint64 bytesSent = 0);
    // Emits pending samples immediately (used when a transfer ends).
    void flush();

signals:
    void flushed(const QString& serviceId,
                 const QString& serviceName,
                 const QString& serviceType,
                 qint64 bytesReceived,
                 qint64 bytesSent);

private:
    struct Pending final {
        QString serviceName;
        QString serviceType;
        qint64 bytesReceived { 0 };
        qint64 bytesSent { 0 };
    };

    QTimer m_timer;
    QHash<QString, Pending> m_pending;
};
