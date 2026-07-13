#include "services/webdav/TransferManager.h"

#include "utils/AppLogger.h"

#include <QAuthenticator>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <ranges>

namespace {
constexpr auto maxConcurrentDownloads = 3;
constexpr auto transferIdleTimeoutMs = 60000;
constexpr auto progressPublishIntervalMs = 120;
constexpr auto speedSampleIntervalMs = 500;

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

bool finishedStatus(const QString& status)
{
    return status == statusDone() || status == statusFailed() || status == statusCanceled();
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
    return m_queue.size() + m_active.size();
}

int TransferManager::completedCount() const
{
    return static_cast<int>(std::ranges::count_if(m_tasks, [](const TransferTask& task) {
        return task.status == statusDone();
    }));
}

int TransferManager::failedCount() const
{
    return static_cast<int>(std::ranges::count_if(m_tasks, [](const TransferTask& task) {
        return task.status == statusFailed() || task.status == statusCanceled();
    }));
}

qint64 TransferManager::bytesPerSecond() const
{
    qint64 total = 0;
    for (const auto& task : m_tasks) {
        if (task.status == statusRunning()) {
            total += std::max<qint64>(0, task.bytesPerSecond);
        }
    }
    return total;
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
    DownloadRequest request {
        .remoteUrl = remoteUrl,
        .localPath = localPath,
        .totalBytes = totalBytes,
    };
    const auto taskId = makeTaskId();

    QueuedTask queued;
    queued.server = server;
    queued.password = password;
    queued.remoteUrl = request.remoteUrl;
    queued.localPath = request.localPath;
    queued.direction = Direction::Download;
    queued.task = TransferTask {
        .id = taskId,
        .title = fileNameFor(request.localPath),
        .direction = directionText(queued.direction),
        .status = statusQueued(),
        .detail = QStringLiteral("Waiting"),
        .source = request.remoteUrl.toString(),
        .target = request.localPath,
        .bytesTotal = request.totalBytes,
    };
    enqueue(std::move(queued));
    return taskId;
}

void TransferManager::enqueueDownloads(const ServerConfig& server,
                                       const QString& password,
                                       std::vector<DownloadRequest> requests)
{
    if (requests.empty()) {
        return;
    }

    std::vector<TransferTask> appendedTasks;
    appendedTasks.reserve(requests.size());
    for (auto& request : requests) {
        QueuedTask queued;
        queued.server = server;
        queued.password = password;
        queued.remoteUrl = std::move(request.remoteUrl);
        queued.localPath = std::move(request.localPath);
        queued.direction = Direction::Download;
        queued.task = TransferTask {
            .id = makeTaskId(),
            .title = fileNameFor(queued.localPath),
            .direction = directionText(queued.direction),
            .status = statusQueued(),
            .detail = QStringLiteral("Waiting"),
            .source = queued.remoteUrl.toString(),
            .target = queued.localPath,
            .bytesTotal = request.totalBytes,
        };
        appendedTasks.push_back(queued.task);
        m_tasks.push_back(queued.task);
        m_queue.enqueue(std::move(queued));
    }

    m_model.appendTasks(std::move(appendedTasks));
    emit tasksChanged();
    startNext();
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
    if (const auto active = m_active.value(taskId)) {
        if (active->reply) {
            active->reply->abort();
        } else {
            finishActive(taskId, false, QStringLiteral("Canceled"));
        }
        return;
    }

    for (auto index = 0; index < m_queue.size(); ++index) {
        if (m_queue.at(index).task.id != taskId) {
            continue;
        }
        auto queued = m_queue.takeAt(index);
        for (auto& task : m_tasks) {
            if (task.id == queued.task.id) {
                task.status = statusCanceled();
                task.detail = QStringLiteral("Canceled");
                task.cancellable = false;
                publishTask(task);
                break;
            }
        }
        emit taskFinished(queued.task.id, false, QStringLiteral("Canceled"));
        startNext();
        return;
    }
}

void TransferManager::clearFinished()
{
    const auto previousSize = m_tasks.size();
    std::erase_if(m_tasks, [](const TransferTask& task) {
        return finishedStatus(task.status);
    });
    if (m_tasks.size() == previousSize) {
        return;
    }
    m_model.setTasks(m_tasks);
    emit tasksChanged();
}

void TransferManager::enqueue(QueuedTask task)
{
    std::vector<TransferTask> appendedTasks { task.task };
    m_tasks.push_back(task.task);
    m_queue.enqueue(std::move(task));
    m_model.appendTasks(std::move(appendedTasks));
    emit tasksChanged();
    startNext();
}

void TransferManager::startNext()
{
    while (!m_queue.isEmpty()) {
        auto exclusiveTransferActive = false;
        for (const auto& active : m_active) {
            if (active->queued.direction != Direction::Download) {
                exclusiveTransferActive = true;
                break;
            }
        }
        if (exclusiveTransferActive) {
            return;
        }

        const auto nextDirection = m_queue.head().direction;
        if (nextDirection != Direction::Download) {
            if (!m_active.isEmpty()) {
                return;
            }
            auto task = m_queue.dequeue();
            startTask(std::move(task));
            return;
        }
        if (m_active.size() >= maxConcurrentDownloads) {
            return;
        }
        auto task = m_queue.dequeue();
        startTask(std::move(task));
    }
}

void TransferManager::startTask(QueuedTask task)
{
    const auto taskId = task.task.id;
    auto active = std::make_shared<ActiveTask>();
    active->queued = std::move(task);
    active->elapsed.start();
    m_active.insert(taskId, active);

    for (auto& existing : m_tasks) {
        if (existing.id == taskId) {
            existing.status = statusRunning();
            existing.detail = QStringLiteral("Running");
            publishTask(existing);
            break;
        }
    }

    QNetworkRequest request(active->queued.remoteUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("vibePlayerQT/0.1"));

    QNetworkReply* reply = nullptr;
    if (active->queued.direction == Direction::CreateDirectory) {
        reply = m_manager.sendCustomRequest(request, QByteArrayLiteral("MKCOL"));
    } else if (active->queued.direction == Direction::Upload) {
        auto* file = new QFile(active->queued.localPath);
        if (!file->open(QIODevice::ReadOnly)) {
            file->deleteLater();
            finishActive(taskId, false, QStringLiteral("Unable to open local file for upload"));
            return;
        }
        reply = m_manager.put(request, file);
        file->setParent(reply);
        active->file = file;
    } else {
        auto* file = new QFile(active->queued.localPath);
        if (!file->open(QIODevice::WriteOnly)) {
            file->deleteLater();
            finishActive(taskId, false, QStringLiteral("Unable to open local file for download"));
            return;
        }
        reply = m_manager.get(request);
        file->setParent(reply);
        active->file = file;
        connect(reply, &QNetworkReply::readyRead, reply, [reply, file]() {
            if (!file->isOpen()) {
                return;
            }
            const auto data = reply->readAll();
            if (!data.isEmpty() && file->write(data) != data.size()) {
                reply->setProperty("transferWriteError", file->errorString());
                reply->abort();
            }
        });
    }

    active->reply = reply;
    reply->setProperty("webdavUsername", active->queued.server.username);
    reply->setProperty("webdavPassword", active->queued.password);
    wireReply(reply, active->queued.server);

    if (active->queued.direction == Direction::Upload) {
        connect(reply, &QNetworkReply::uploadProgress, reply, [this, taskId](qint64 done, qint64 total) {
            updateProgress(taskId, done, total);
        });
    } else if (active->queued.direction == Direction::Download) {
        connect(reply, &QNetworkReply::downloadProgress, reply, [this, taskId](qint64 done, qint64 total) {
            updateProgress(taskId, done, total);
        });
    }

    const auto direction = active->queued.direction;
    connect(reply, &QNetworkReply::finished, reply, [this, reply, taskId, direction]() {
        const auto writeError = reply->property("transferWriteError").toString();
        const auto timedOut = reply->property("transferTimedOut").toBool();
        const auto canceled = reply->error() == QNetworkReply::OperationCanceledError && !timedOut && writeError.isEmpty();
        const auto statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto existingDirectory = direction == Direction::CreateDirectory && statusCode == 405;

        if (!writeError.isEmpty()) {
            finishActive(taskId, false, writeError);
        } else if (timedOut) {
            finishActive(taskId, false, QStringLiteral("Transfer timed out"));
        } else if (reply->error() != QNetworkReply::NoError && !existingDirectory) {
            finishActive(taskId, false, canceled ? QStringLiteral("Canceled") : reply->errorString());
        } else {
            finishActive(taskId, true, QStringLiteral("Completed"));
        }
        reply->deleteLater();
    });

    AppLogger::info(QStringLiteral("webdav-transfer"),
                    QStringLiteral("Started %1 task: %2")
                        .arg(active->queued.task.direction, active->queued.task.title));
}

void TransferManager::publishTask(const TransferTask& task)
{
    m_model.updateTask(task);
    emit tasksChanged();
}

void TransferManager::updateProgress(const QString& taskId, qint64 done, qint64 total)
{
    const auto active = m_active.value(taskId);
    if (!active) {
        return;
    }

    if (active->queued.direction == Direction::Upload) {
        const auto delta = done - active->queued.countedBytesSent;
        if (delta > 0) {
            active->queued.countedBytesSent = done;
            emit networkTrafficSample(active->queued.server.id,
                                      active->queued.server.name,
                                      serviceTypeToString(active->queued.server.serviceType),
                                      0,
                                      delta);
        }
    } else if (active->queued.direction == Direction::Download) {
        const auto delta = done - active->queued.countedBytesReceived;
        if (delta > 0) {
            active->queued.countedBytesReceived = done;
            emit networkTrafficSample(active->queued.server.id,
                                      active->queued.server.name,
                                      serviceTypeToString(active->queued.server.serviceType),
                                      delta,
                                      0);
        }
    }

    for (auto& task : m_tasks) {
        if (task.id != taskId) {
            continue;
        }

        task.bytesDone = std::max<qint64>(0, done);
        task.bytesTotal = total > 0 ? total : task.bytesTotal;
        task.progress = task.bytesTotal > 0
            ? std::clamp(static_cast<double>(task.bytesDone) / static_cast<double>(task.bytesTotal), 0.0, 1.0)
            : 0.0;

        const auto elapsedMs = active->elapsed.elapsed();
        const auto sampleDurationMs = elapsedMs - active->speedSampleElapsedMs;
        if (sampleDurationMs >= speedSampleIntervalMs) {
            const auto sampleBytes = task.bytesDone - active->speedSampleBytes;
            task.bytesPerSecond = sampleBytes > 0
                ? sampleBytes * 1000 / sampleDurationMs
                : 0;
            active->speedSampleBytes = task.bytesDone;
            active->speedSampleElapsedMs = elapsedMs;
        }

        const auto finishedProgress = task.bytesTotal > 0 && task.bytesDone >= task.bytesTotal;
        if (finishedProgress || elapsedMs - active->lastPublishedElapsedMs >= progressPublishIntervalMs) {
            active->lastPublishedElapsedMs = elapsedMs;
            publishTask(task);
        }
        return;
    }
}

void TransferManager::finishActive(const QString& taskId, bool ok, const QString& message)
{
    const auto active = m_active.value(taskId);
    if (!active) {
        return;
    }

    if (active->file) {
        active->file->close();
    }
    if (!ok && active->queued.direction == Direction::Download) {
        QFile::remove(active->queued.localPath);
    }
    m_active.remove(taskId);

    const auto status = ok ? statusDone() : (message == QStringLiteral("Canceled") ? statusCanceled() : statusFailed());
    for (auto& task : m_tasks) {
        if (task.id != taskId) {
            continue;
        }
        task.status = status;
        task.detail = message;
        task.progress = ok ? 1.0 : task.progress;
        if (ok && task.bytesTotal > 0) {
            task.bytesDone = task.bytesTotal;
        }
        task.bytesPerSecond = 0;
        task.cancellable = false;
        publishTask(task);
        break;
    }

    if (ok) {
        AppLogger::info(QStringLiteral("webdav-transfer"),
                        QStringLiteral("Completed %1 task: %2")
                            .arg(active->queued.task.direction, active->queued.task.title));
    } else {
        AppLogger::warning(QStringLiteral("webdav-transfer"),
                           QStringLiteral("%1 task failed: %2")
                               .arg(active->queued.task.direction, message));
    }

    emit taskFinished(taskId, ok, message);
    QTimer::singleShot(0, this, [this]() {
        startNext();
    });
}

void TransferManager::wireReply(QNetworkReply* reply, const ServerConfig& server)
{
    auto* idleTimer = new QTimer(reply);
    idleTimer->setSingleShot(true);
    idleTimer->setInterval(transferIdleTimeoutMs);
    connect(idleTimer, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("transferTimedOut", true);
        reply->abort();
    });
    connect(reply, &QNetworkReply::readyRead, idleTimer, qOverload<>(&QTimer::start));
    connect(reply, &QNetworkReply::downloadProgress, idleTimer, [idleTimer](qint64, qint64) {
        idleTimer->start();
    });
    connect(reply, &QNetworkReply::uploadProgress, idleTimer, [idleTimer](qint64, qint64) {
        idleTimer->start();
    });
    idleTimer->start();

    connect(reply, &QNetworkReply::sslErrors, reply, [reply, allowSelfSigned = server.trustSelfSignedCertificate](const QList<QSslError>&) {
        if (allowSelfSigned) {
            reply->ignoreSslErrors();
        }
    });
}
