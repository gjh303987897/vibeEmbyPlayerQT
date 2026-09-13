#pragma once

#include "models/ServerConfig.h"
#include "models/TransferTask.h"
#include "utils/NetworkTrafficCoalescer.h"
#include "viewmodels/TransferTaskListModel.h"

#include <QFile>
#include <QElapsedTimer>
#include <QHash>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QTimer>
#include <QUrl>

#include <functional>
#include <memory>
#include <vector>

class TransferManager final : public QObject {
    Q_OBJECT

public:
    struct TaskRequest {
        QUrl remoteUrl;
        QString localPath;
        qint64 totalBytes { -1 };
        // Child title inside a group; empty falls back to the local file name.
        // Upload rows must carry a title because their target is a URL.
        QString title;
        // MKCOL child for directory-creating entries in an upload batch.
        bool directory { false };
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
    qint64 remainingBytesForDirection(const QString& direction) const;
    // True while at least one top-level task of the direction is unfinished; drives
    // the remaining-bytes tile's download/upload rotation.
    bool hasActiveDirection(const QString& direction) const;

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
                             std::vector<TaskRequest> requests);
    // Batch upload: each request uploads to `remoteUrl` and carries its own
    // `title` (used verbatim for children, so upload rows never leak the URL);
    // `directory` requests become MKCOL children. One file request behaves
    // exactly like enqueueUpload(); several form a folder group (summary +
    // expandable children) just like download groups.
    QString enqueueUploads(const ServerConfig& server,
                           const QString& password,
                           const QString& groupTitle,
                           const QString& groupTarget,
                           std::vector<TaskRequest> requests);
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
    void taskProgress(const QString& taskId, qint64 bytesDone, qint64 bytesTotal);
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
        int automaticRetryCount { 0 };
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
        QPointer<QTimer> retryTimer;
    };

    // One group state per folder task (download or upload). Upload groups reuse
    // the whole cancel/pause/resume/retry/aggregate pipeline; the isUpload flag
    // only guards local-file cleanup (an upload must never delete its source).
    struct GroupState {
        QString id;
        bool isUpload { false };
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
    std::shared_ptr<GroupState> groupById(const QString& groupId) const;
    void cancelGroup(const QString& groupId);
    void pauseGroup(const QString& groupId);
    void resumeGroup(const QString& groupId);
    void retryGroup(const QString& groupId);
    void startNext();
    void startTask(QueuedTask task);
    void publishTask(const TransferTask& task);
    void updateGroup(const QString& groupId);
    void updateProgress(const QString& taskId, qint64 done, qint64 total);
    void finishActive(const QString& taskId, bool ok, const QString& message);
    void finishPaused(const QString& taskId);
    void wireReply(QNetworkReply* reply, const ServerConfig& server);
    bool scheduleAutomaticRetry(const QString& taskId,
                                const QString& errorMessage,
                                int statusCode);
    bool requeueTask(const QString& taskId);
    bool prepareGroupRetry(const QString& groupId);
    void cleanupGroupFiles(const QString& groupId);
    void startGroupTimer(const std::shared_ptr<GroupState>& group);
    void stopGroupTimer(const std::shared_ptr<GroupState>& group);
    qint64 groupElapsedMs(const std::shared_ptr<GroupState>& group) const;
    qint64 rateForDirection(const QString& direction, bool average) const;

    QNetworkAccessManager m_manager;
    NetworkTrafficCoalescer m_traffic;
    TransferTaskListModel m_model;
    TransferTaskListModel m_detailModel;
    std::vector<TransferTask> m_topLevelTasks;
    std::vector<TransferTask> m_tasks;
    QQueue<QueuedTask> m_queue;
    QHash<QString, std::shared_ptr<ActiveTask>> m_active;
    QHash<QString, QueuedTask> m_taskDefinitions;
    QHash<QString, std::shared_ptr<GroupState>> m_downloadGroups;
    QHash<QString, std::shared_ptr<GroupState>> m_uploadGroups;
    QString m_selectedGroupId;
};
