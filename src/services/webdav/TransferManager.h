#pragma once

#include "models/ServerConfig.h"
#include "models/TransferTask.h"
#include "viewmodels/TransferTaskListModel.h"

#include <QFile>
#include <QNetworkAccessManager>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSslError>
#include <QUrl>

#include <functional>
#include <optional>
#include <vector>

class TransferManager final : public QObject {
    Q_OBJECT

public:
    enum class Direction {
        Upload,
        Download,
        CreateDirectory,
    };

    explicit TransferManager(QObject* parent = nullptr);

    TransferTaskListModel* tasks();
    int activeCount() const;

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
    QString enqueueCreateDirectory(const ServerConfig& server,
                                   const QString& password,
                                   const QUrl& remoteUrl);

    Q_INVOKABLE void cancelTask(const QString& taskId);

signals:
    void tasksChanged();
    void taskFinished(const QString& taskId, bool ok, const QString& message);
    void certificateConfirmationRequired(const QString& host, const QList<QSslError>& errors, std::function<void(bool)> reply);

private:
    struct QueuedTask {
        TransferTask task;
        ServerConfig server;
        QString password;
        QUrl remoteUrl;
        QString localPath;
        Direction direction { Direction::Download };
    };

    void enqueue(QueuedTask task);
    void startNext();
    void refreshModel();
    void updateProgress(const QString& taskId, qint64 done, qint64 total);
    void finishActive(const QString& taskId, bool ok, const QString& message);
    void wireReply(QNetworkReply* reply, const ServerConfig& server);

    QNetworkAccessManager m_manager;
    TransferTaskListModel m_model;
    std::vector<TransferTask> m_tasks;
    QQueue<QueuedTask> m_queue;
    std::optional<QueuedTask> m_active;
    QPointer<QNetworkReply> m_activeReply;
    QPointer<QFile> m_activeFile;
};
