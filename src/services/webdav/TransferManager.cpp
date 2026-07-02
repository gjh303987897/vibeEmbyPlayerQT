#include "services/webdav/TransferManager.h"

#include <QAuthenticator>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>

namespace {
QString makeTaskId()
{
    return QStringLiteral("transfer-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

QString fileNameFor(const QString& path)
{
    const QFileInfo info(path);
    return info.fileName().isEmpty() ? path : info.fileName();
}

QString directionText(TransferManager::Direction direction)
{
    switch (direction) {
    case TransferManager::Direction::Upload:
        return QStringLiteral("upload");
    case TransferManager::Direction::Download:
        return QStringLiteral("download");
    case TransferManager::Direction::CreateDirectory:
        return QStringLiteral("mkdir");
    }
    return QStringLiteral("transfer");
}

QString statusQueued()
{
    return QStringLiteral("queued");
}

QString statusRunning()
{
    return QStringLiteral("running");
}

QString statusDone()
{
    return QStringLiteral("done");
}

QString statusFailed()
{
    return QStringLiteral("failed");
}

QString statusCanceled()
{
    return QStringLiteral("canceled");
}
}

TransferManager::TransferManager(QObject* parent)
    : QObject(parent)
{
    connect(&m_manager, &QNetworkAccessManager::authenticationRequired, this, [](QNetworkReply* reply, QAuthenticator* authenticator) {
        authenticator->setUser(reply->property("webdavUsername").toString());
        authenticator->setPassword(reply->property("webdavPassword").toString());
    });
}

TransferTaskListModel* TransferManager::tasks()
{
    return &m_model;
}

int TransferManager::activeCount() const
{
    auto count = m_queue.size();
    if (m_active) {
        ++count;
    }
    return count;
}

QString TransferManager::enqueueUpload(const ServerConfig& server,
                                       const QString& password,
                                       const QString& localPath,
                                       const QUrl& remoteUrl,
                                       qint64 totalBytes)
{
    QueuedTask queued;
    queued.server = server;
    queued.password = password;
    queued.remoteUrl = remoteUrl;
    queued.localPath = localPath;
    queued.direction = Direction::Upload;
    queued.task = TransferTask {
        .id = makeTaskId(),
        .title = fileNameFor(localPath),
        .direction = directionText(queued.direction),
        .status = statusQueued(),
        .detail = QStringLiteral("Waiting"),
        .source = localPath,
        .target = remoteUrl.toString(),
        .bytesTotal = totalBytes,
    };
    const auto id = queued.task.id;
    enqueue(std::move(queued));
    return id;
}

QString TransferManager::enqueueDownload(const ServerConfig& server,
                                         const QString& password,
                                         const QUrl& remoteUrl,
                                         const QString& localPath,
                                         qint64 totalBytes)
{
    QueuedTask queued;
    queued.server = server;
    queued.password = password;
    queued.remoteUrl = remoteUrl;
    queued.localPath = localPath;
    queued.direction = Direction::Download;
    queued.task = TransferTask {
        .id = makeTaskId(),
        .title = fileNameFor(localPath),
        .direction = directionText(queued.direction),
        .status = statusQueued(),
        .detail = QStringLiteral("Waiting"),
        .source = remoteUrl.toString(),
        .target = localPath,
        .bytesTotal = totalBytes,
    };
    const auto id = queued.task.id;
    enqueue(std::move(queued));
    return id;
}

QString TransferManager::enqueueCreateDirectory(const ServerConfig& server,
                                                const QString& password,
                                                const QUrl& remoteUrl)
{
    QueuedTask queued;
    queued.server = server;
    queued.password = password;
    queued.remoteUrl = remoteUrl;
    queued.direction = Direction::CreateDirectory;
    queued.task = TransferTask {
        .id = makeTaskId(),
        .title = remoteUrl.path().section(QLatin1Char('/'), -2, -2),
        .direction = directionText(queued.direction),
        .status = statusQueued(),
        .detail = QStringLiteral("Waiting"),
        .source = QString {},
        .target = remoteUrl.toString(),
        .bytesTotal = 0,
        .progress = 0.0,
    };
    const auto id = queued.task.id;
    enqueue(std::move(queued));
    return id;
}

void TransferManager::cancelTask(const QString& taskId)
{
    if (m_active && m_active->task.id == taskId) {
        if (m_activeReply) {
            m_activeReply->abort();
        }
        return;
    }

    for (int i = 0; i < m_queue.size(); ++i) {
        if (m_queue.at(i).task.id == taskId) {
            auto task = m_queue.takeAt(i);
            const auto message = QStringLiteral("Canceled");
            for (auto& existing : m_tasks) {
                if (existing.id == task.task.id) {
                    existing.status = statusCanceled();
                    existing.detail = message;
                    existing.cancellable = false;
                    break;
                }
            }
            refreshModel();
            emit taskFinished(task.task.id, false, message);
            return;
        }
    }
}

void TransferManager::enqueue(QueuedTask task)
{
    m_tasks.push_back(task.task);
    m_queue.enqueue(std::move(task));
    refreshModel();
    startNext();
}

void TransferManager::startNext()
{
    if (m_active || m_queue.isEmpty()) {
        return;
    }

    m_active = m_queue.dequeue();
    for (auto& task : m_tasks) {
        if (task.id == m_active->task.id) {
            task.status = statusRunning();
            task.detail = QStringLiteral("Running");
            break;
        }
    }
    refreshModel();

    QNetworkRequest request(m_active->remoteUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT/0.1"));

    QNetworkReply* reply = nullptr;
    if (m_active->direction == Direction::CreateDirectory) {
        reply = m_manager.sendCustomRequest(request, QByteArrayLiteral("MKCOL"));
    } else if (m_active->direction == Direction::Upload) {
        auto* file = new QFile(m_active->localPath);
        if (!file->open(QIODevice::ReadOnly)) {
            file->deleteLater();
            finishActive(m_active->task.id, false, QStringLiteral("Unable to open local file for upload"));
            return;
        }
        reply = m_manager.put(request, file);
        file->setParent(reply);
        m_activeFile = file;
    } else {
        auto* file = new QFile(m_active->localPath);
        if (!file->open(QIODevice::WriteOnly)) {
            file->deleteLater();
            finishActive(m_active->task.id, false, QStringLiteral("Unable to open local file for download"));
            return;
        }
        reply = m_manager.get(request);
        file->setParent(reply);
        m_activeFile = file;
        connect(reply, &QNetworkReply::readyRead, reply, [reply, file]() {
            if (file->isOpen()) {
                file->write(reply->readAll());
            }
        });
    }

    m_activeReply = reply;
    reply->setProperty("webdavUsername", m_active->server.username);
    reply->setProperty("webdavPassword", m_active->password);
    wireReply(reply, m_active->server);

    reply->setProperty("transferTaskId", m_active->task.id);

    connect(reply, &QNetworkReply::uploadProgress, reply, [this, taskId = m_active->task.id](qint64 done, qint64 total) {
        updateProgress(taskId, done, total);
    });
    connect(reply, &QNetworkReply::downloadProgress, reply, [this, taskId = m_active->task.id](qint64 done, qint64 total) {
        updateProgress(taskId, done, total);
    });

    connect(reply, &QNetworkReply::finished, reply, [this, reply]() {
        const auto taskId = reply->property("transferTaskId").toString();
        const auto canceled = reply->error() == QNetworkReply::OperationCanceledError;
        const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError && statusCode != 405) {
            finishActive(taskId, false, canceled ? QStringLiteral("Canceled") : reply->errorString());
            reply->deleteLater();
            return;
        }
        finishActive(taskId, true, QStringLiteral("Completed"));
        reply->deleteLater();
    });
}

void TransferManager::refreshModel()
{
    m_model.setTasks(m_tasks);
    emit tasksChanged();
}

void TransferManager::updateProgress(const QString& taskId, qint64 done, qint64 total)
{
    if (m_active && m_active->task.id == taskId) {
        if (m_active->direction == Direction::Upload) {
            const auto delta = done - m_active->countedBytesSent;
            if (delta > 0) {
                m_active->countedBytesSent = done;
                emit networkTrafficSample(m_active->server.id,
                                          m_active->server.name,
                                          serviceTypeToString(m_active->server.serviceType),
                                          0,
                                          delta);
            }
        } else if (m_active->direction == Direction::Download) {
            const auto delta = done - m_active->countedBytesReceived;
            if (delta > 0) {
                m_active->countedBytesReceived = done;
                emit networkTrafficSample(m_active->server.id,
                                          m_active->server.name,
                                          serviceTypeToString(m_active->server.serviceType),
                                          delta,
                                          0);
            }
        }
    }

    for (auto& task : m_tasks) {
        if (task.id == taskId) {
            task.bytesDone = done;
            task.bytesTotal = total > 0 ? total : task.bytesTotal;
            task.progress = task.bytesTotal > 0 ? static_cast<double>(done) / static_cast<double>(task.bytesTotal) : 0.0;
            refreshModel();
            return;
        }
    }
}

void TransferManager::finishActive(const QString& taskId, bool ok, const QString& message)
{
    if (!m_active || m_active->task.id != taskId) {
        return;
    }

    if (m_activeFile) {
        m_activeFile->close();
        m_activeFile.clear();
    }

    const auto status = ok ? statusDone() : (message == QStringLiteral("Canceled") ? statusCanceled() : statusFailed());
    for (auto& task : m_tasks) {
        if (task.id == taskId) {
            task.status = status;
            task.detail = message;
            task.progress = ok ? 1.0 : task.progress;
            task.cancellable = false;
            break;
        }
    }

    m_activeReply.clear();
    m_active.reset();
    refreshModel();
    emit taskFinished(taskId, ok, message);
    startNext();
}

void TransferManager::wireReply(QNetworkReply* reply, const ServerConfig& server)
{
    connect(reply, &QNetworkReply::sslErrors, reply, [reply, allowSelfSigned = server.trustSelfSignedCertificate](const QList<QSslError>&) {
        if (!allowSelfSigned) {
            return;
        }
        reply->ignoreSslErrors();
    });
}
