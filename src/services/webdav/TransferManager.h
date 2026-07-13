#pragma once

#include "models/ServerConfig.h"
#include "models/TransferTask.h"
#include "viewmodels/TransferTaskListModel.h"

#include <QFile>
#include <QElapsedTimer>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSslError>
#include <QUrl>

#include <functional>
#include <memory>
#include <vector>

class TransferManager final : public QObject {
    Q_OBJECT

public:
    struct DownloadRequest {
        QUrl remoteUrl;
        QString localPath;
        qint64 totalBytes { -1 };
    };

    enum class Direction {
        Upload,
        Download,
        CreateDirectory,
    };

    explicit TransferManager(QObject* parent = nullptr);

    TransferTaskListModel* tasks();
    int activeCount() const;
    int completedCount() const;
    int failedCount() const;
    qint64 bytesPerSecond() const;

    QString enqueueUpload(const ServerConfig& server,
                          const QString& password,
                          const QString& localPath,
                          const QUrl& remoteUrl,
                          qint64 totalBytes);
    QString enqueueDownload(const ServerConfig& server,
                            const QString& password,
                            const QUrl& remoteUrl,
                            const QString& localPath,
                            qint64 totalBytes);
    void enqueueDownloads(const ServerConfig& server,
                          const QString& password,
                          std::vector<DownloadRequest> requests);
    QString enqueueCreateDirectory(const ServerConfig& server,
                                   const QString& password,
                                   const QUrl& remoteUrl);

    Q_INVOKABLE void cancelTask(const QString& taskId);
    Q_INVOKABLE void clearFinished();

signals:
    void tasksChanged();
    void taskFinished(const QString& taskId, bool ok, const QString& message);
    void certificateConfirmationRequired(const QString& host, const QList<QSslError>& errors, std::function<void(bool)> reply);
    void networkTrafficSample(const QString& serviceId,
                              const QString& serviceName,
                              const QString& serviceType,
                              qint64 bytesReceived,
                              qint64 bytesSent);

private:
    struct QueuedTask {
        TransferTask task;
        ServerConfig server;
        QString password;
        QUrl remoteUrl;
        QString localPath;
        Direction direction { Direction::Download };
        qint64 countedBytesReceived { 0 };
        qint64 countedBytesSent { 0 };
    };

    struct ActiveTask {
        QueuedTask queued;
        QPointer<QNetworkReply> reply;
        QPointer<QFile> file;
        QElapsedTimer elapsed;
        qint64 speedSampleBytes { 0 };
        qint64 speedSampleElapsedMs { 0 };
        qint64 lastPublishedElapsedMs { 0 };
    };

    void enqueue(QueuedTask task);
    void startNext();
    void startTask(QueuedTask task);
    void publishTask(const TransferTask& task);
    void updateProgress(const QString& taskId, qint64 done, qint64 total);
    void finishActive(const QString& taskId, bool ok, const QString& message);
    void wireReply(QNetworkReply* reply, const ServerConfig& server);

    QNetworkAccessManager m_manager;
    TransferTaskListModel m_model;
    std::vector<TransferTask> m_tasks;
    QQueue<QueuedTask> m_queue;
    QHash<QString, std::shared_ptr<ActiveTask>> m_active;
};
