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
    TransferTaskListModel* detailTasks();
    QString selectedGroupId() const;
    QString selectedGroupTitle() const;
    int activeCount() const;
    int completedCount() const;
    int failedCount() const;
    qint64 bytesPerSecond() const;
    qint64 averageBytesPerSecond() const;
    qint64 downloadBytesPerSecond() const;
    qint64 uploadBytesPerSecond() const;
    qint64 averageDownloadBytesPerSecond() const;
    qint64 averageUploadBytesPerSecond() const;
    qint64 remainingBytes() const;

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
    QString enqueueDownloads(const ServerConfig& server,
                             const QString& password,
                             const QString& groupTitle,
                             const QString& groupTarget,
                             std::vector<DownloadRequest> requests);
    QString enqueueCreateDirectory(const ServerConfig& server,
                                   const QString& password,
                                   const QUrl& remoteUrl);

    Q_INVOKABLE void cancelTask(const QString& taskId);
    Q_INVOKABLE void pauseTask(const QString& taskId);
    Q_INVOKABLE void resumeTask(const QString& taskId);
    Q_INVOKABLE void retryTask(const QString& taskId);
    Q_INVOKABLE void clearFinished();
    bool selectGroup(const QString& groupId);
    void clearGroupSelection();

signals:
    void tasksChanged();
    void selectionChanged();
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

    enum class RequestedStop {
        None,
        Pause,
        Cancel,
    };

    struct ActiveTask {
        QueuedTask queued;
        QPointer<QNetworkReply> reply;
        QPointer<QFile> file;
        QElapsedTimer elapsed;
        qint64 speedSampleBytes { 0 };
        qint64 speedSampleElapsedMs { 0 };
        qint64 lastPublishedElapsedMs { 0 };
        RequestedStop requestedStop { RequestedStop::None };
    };

    struct DownloadGroupState {
        QString id;
        QString targetPath;
        std::vector<QString> taskIds;
        QElapsedTimer elapsed;
        qint64 elapsedBeforeCurrentSegmentMs { 0 };
        bool started { false };
        bool timerRunning { false };
        bool pauseRequested { false };
        bool cancelRequested { false };
        bool targetIsDirectory { false };
        bool cleanupCompleted { false };
    };

    void enqueue(QueuedTask task);
    void cancelDownloadGroup(const QString& groupId);
    void pauseDownloadGroup(const QString& groupId);
    void resumeDownloadGroup(const QString& groupId);
    void retryDownloadGroup(const QString& groupId);
    void startNext();
    void startTask(QueuedTask task);
    void publishTask(const TransferTask& task);
    void updateDownloadGroup(const QString& groupId);
    void updateProgress(const QString& taskId, qint64 done, qint64 total);
    void finishActive(const QString& taskId, bool ok, const QString& message);
    void finishPaused(const QString& taskId);
    void wireReply(QNetworkReply* reply, const ServerConfig& server);
    bool requeueTask(const QString& taskId);
    bool prepareDownloadGroupRetry(const QString& groupId);
    void cleanupDownloadGroupFiles(const QString& groupId);
    void startDownloadGroupTimer(const std::shared_ptr<DownloadGroupState>& group);
    void stopDownloadGroupTimer(const std::shared_ptr<DownloadGroupState>& group);
    qint64 downloadGroupElapsedMs(const std::shared_ptr<DownloadGroupState>& group) const;
    qint64 rateForDirection(const QString& direction, bool average) const;

    QNetworkAccessManager m_manager;
    TransferTaskListModel m_model;
    TransferTaskListModel m_detailModel;
    std::vector<TransferTask> m_topLevelTasks;
    std::vector<TransferTask> m_tasks;
    QQueue<QueuedTask> m_queue;
    QHash<QString, std::shared_ptr<ActiveTask>> m_active;
    QHash<QString, QueuedTask> m_taskDefinitions;
    QHash<QString, std::shared_ptr<DownloadGroupState>> m_downloadGroups;
    QString m_selectedGroupId;
};
