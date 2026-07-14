#include "services/webdav/TransferManager.h"

#include "utils/AppLogger.h"

#include <QAuthenticator>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSet>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <limits>
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

QString makeGroupId()
{
    return QStringLiteral("transfer-group-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
}

qint64 averageRate(qint64 bytes, qint64 elapsedMs)
{
    if (bytes <= 0 || elapsedMs <= 0) {
        return 0;
    }
    const auto rate = static_cast<long double>(bytes) * 1000.0L / static_cast<long double>(elapsedMs);
    return static_cast<qint64>(std::min(rate,
                                        static_cast<long double>(std::numeric_limits<qint64>::max())));
}

bool checkedAddBytes(qint64& total, qint64 value)
{
    if (value < 0 || total > std::numeric_limits<qint64>::max() - value) {
        return false;
    }
    total += value;
    return true;
}

void saturatingAddBytes(qint64& total, qint64 value)
{
    if (value <= 0) {
        return;
    }
    if (!checkedAddBytes(total, value)) {
        total = std::numeric_limits<qint64>::max();
    }
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

TransferTaskListModel* TransferManager::detailTasks()
{
    return &m_detailModel;
}

QString TransferManager::selectedGroupId() const
{
    return m_selectedGroupId;
}

QString TransferManager::selectedGroupTitle() const
{
    const auto summary = std::ranges::find_if(m_topLevelTasks, [this](const TransferTask& task) {
        return task.id == m_selectedGroupId;
    });
    return summary == m_topLevelTasks.end() ? QString {} : summary->title;
}

int TransferManager::activeCount() const
{
    return static_cast<int>(std::ranges::count_if(m_topLevelTasks, [](const TransferTask& task) {
        return !finishedStatus(task.status);
    }));
}

int TransferManager::completedCount() const
{
    return static_cast<int>(std::ranges::count_if(m_topLevelTasks, [](const TransferTask& task) {
        return task.status == statusDone();
    }));
}

int TransferManager::failedCount() const
{
    return static_cast<int>(std::ranges::count_if(m_topLevelTasks, [](const TransferTask& task) {
        return task.status == statusFailed() || task.status == statusCanceled();
    }));
}

qint64 TransferManager::bytesPerSecond() const
{
    auto total = downloadBytesPerSecond();
    saturatingAddBytes(total, uploadBytesPerSecond());
    return total;
}

qint64 TransferManager::averageBytesPerSecond() const
{
    auto total = averageDownloadBytesPerSecond();
    saturatingAddBytes(total, averageUploadBytesPerSecond());
    return total;
}

qint64 TransferManager::downloadBytesPerSecond() const
{
    return rateForDirection(QStringLiteral("download"), false);
}

qint64 TransferManager::uploadBytesPerSecond() const
{
    return rateForDirection(QStringLiteral("upload"), false);
}

qint64 TransferManager::averageDownloadBytesPerSecond() const
{
    return rateForDirection(QStringLiteral("download"), true);
}

qint64 TransferManager::averageUploadBytesPerSecond() const
{
    return rateForDirection(QStringLiteral("upload"), true);
}

qint64 TransferManager::rateForDirection(const QString& direction, bool average) const
{
    qint64 total = 0;
    for (const auto& task : m_topLevelTasks) {
        const auto included = average ? !finishedStatus(task.status) : task.status == statusRunning();
        if (included && task.direction == direction) {
            saturatingAddBytes(total, average ? task.averageBytesPerSecond : task.bytesPerSecond);
        }
    }
    return total;
}

qint64 TransferManager::remainingBytes() const
{
    qint64 total = 0;
    for (const auto& task : m_topLevelTasks) {
        if (task.direction != QStringLiteral("download") || finishedStatus(task.status)) {
            continue;
        }
        if (task.bytesRemaining < 0) {
            return -1;
        }
        if (!checkedAddBytes(total, task.bytesRemaining)) {
            return -1;
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
    std::vector<DownloadRequest> requests;
    requests.push_back(DownloadRequest {
        .remoteUrl = remoteUrl,
        .localPath = localPath,
        .totalBytes = totalBytes,
    });
    return enqueueDownloads(server,
                            password,
                            fileNameFor(localPath),
                            localPath,
                            std::move(requests));
}

QString TransferManager::enqueueDownloads(const ServerConfig& server,
                                          const QString& password,
                                          const QString& groupTitle,
                                          const QString& groupTarget,
                                          std::vector<DownloadRequest> requests)
{
    const auto groupId = makeGroupId();
    auto group = std::make_shared<DownloadGroupState>();
    group->id = groupId;
    group->taskIds.reserve(requests.size());

    qint64 totalBytes = 0;
    auto completeSize = true;
    for (const auto& request : requests) {
        if (!checkedAddBytes(totalBytes, request.totalBytes)) {
            completeSize = false;
        }
    }

    TransferTask summary {
        .id = groupId,
        .title = groupTitle,
        .direction = directionText(Direction::Download),
        .status = requests.empty() ? statusDone() : statusQueued(),
        .detail = QStringLiteral("0 / %1 files").arg(requests.size()),
        .target = groupTarget,
        .bytesTotal = completeSize ? totalBytes : -1,
        .bytesRemaining = completeSize ? totalBytes : -1,
        .progress = requests.empty() ? 1.0 : 0.0,
        .fileCount = static_cast<int>(requests.size()),
        .isGroup = true,
        .cancellable = !requests.empty(),
    };

    for (auto& request : requests) {
        QueuedTask queued;
        queued.server = server;
        queued.password = password;
        queued.remoteUrl = std::move(request.remoteUrl);
        queued.localPath = std::move(request.localPath);
        queued.direction = Direction::Download;
        queued.task = TransferTask {
            .id = makeTaskId(),
            .parentId = groupId,
            .title = fileNameFor(queued.localPath),
            .direction = directionText(queued.direction),
            .status = statusQueued(),
            .detail = QStringLiteral("Waiting"),
            .source = queued.remoteUrl.toString(),
            .target = queued.localPath,
            .bytesTotal = request.totalBytes,
            .bytesRemaining = request.totalBytes,
        };
        group->taskIds.push_back(queued.task.id);
        m_tasks.push_back(queued.task);
        m_queue.enqueue(std::move(queued));
    }

    m_downloadGroups.insert(groupId, std::move(group));
    m_topLevelTasks.push_back(summary);
    m_model.appendTasks(std::vector<TransferTask> { std::move(summary) });
    emit tasksChanged();
    startNext();
    return groupId;
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
    if (m_downloadGroups.contains(taskId)) {
        cancelDownloadGroup(taskId);
        return;
    }

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

void TransferManager::cancelDownloadGroup(const QString& groupId)
{
    const auto group = m_downloadGroups.value(groupId);
    if (!group) {
        return;
    }

    const QSet<QString> taskIds(group->taskIds.begin(), group->taskIds.end());
    for (auto index = m_queue.size() - 1; index >= 0; --index) {
        if (!taskIds.contains(m_queue.at(index).task.id)) {
            continue;
        }
        const auto queued = m_queue.takeAt(index);
        for (auto& task : m_tasks) {
            if (task.id != queued.task.id) {
                continue;
            }
            task.status = statusCanceled();
            task.detail = QStringLiteral("Canceled");
            task.cancellable = false;
            publishTask(task);
            break;
        }
        emit taskFinished(queued.task.id, false, QStringLiteral("Canceled"));
    }

    for (const auto& taskId : group->taskIds) {
        const auto active = m_active.value(taskId);
        if (!active) {
            continue;
        }
        if (active->reply) {
            active->reply->abort();
        } else {
            finishActive(taskId, false, QStringLiteral("Canceled"));
        }
    }
    updateDownloadGroup(groupId);
    emit tasksChanged();
    startNext();
}

void TransferManager::clearFinished()
{
    QSet<QString> removedIds;
    for (const auto& task : m_topLevelTasks) {
        if (finishedStatus(task.status)) {
            removedIds.insert(task.id);
        }
    }
    if (removedIds.isEmpty()) {
        return;
    }

    std::erase_if(m_topLevelTasks, [&removedIds](const TransferTask& task) {
        return removedIds.contains(task.id);
    });
    std::erase_if(m_tasks, [&removedIds](const TransferTask& task) {
        return removedIds.contains(task.id) || removedIds.contains(task.parentId);
    });
    for (const auto& id : removedIds) {
        m_downloadGroups.remove(id);
    }
    m_model.setTasks(m_topLevelTasks);

    if (removedIds.contains(m_selectedGroupId)) {
        m_selectedGroupId.clear();
        m_detailModel.setTasks({});
        emit selectionChanged();
    }
    emit tasksChanged();
}

bool TransferManager::selectGroup(const QString& groupId)
{
    if (!m_downloadGroups.contains(groupId)) {
        return false;
    }

    std::vector<TransferTask> details;
    const auto group = m_downloadGroups.value(groupId);
    details.reserve(group->taskIds.size());
    for (const auto& task : m_tasks) {
        if (task.parentId == groupId) {
            details.push_back(task);
        }
    }
    m_selectedGroupId = groupId;
    m_detailModel.setTasks(std::move(details));
    emit selectionChanged();
    return true;
}

void TransferManager::clearGroupSelection()
{
    if (m_selectedGroupId.isEmpty()) {
        return;
    }
    m_selectedGroupId.clear();
    m_detailModel.setTasks({});
    emit selectionChanged();
}

void TransferManager::enqueue(QueuedTask task)
{
    std::vector<TransferTask> appendedTasks { task.task };
    m_tasks.push_back(task.task);
    m_topLevelTasks.push_back(task.task);
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

    if (!active->queued.task.parentId.isEmpty()) {
        const auto group = m_downloadGroups.value(active->queued.task.parentId);
        if (group && !group->started) {
            group->elapsed.start();
            group->started = true;
        }
    }

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
    m_detailModel.updateTask(task);
    if (task.parentId.isEmpty()) {
        const auto topLevel = std::ranges::find_if(m_topLevelTasks, [&task](const TransferTask& existing) {
            return existing.id == task.id;
        });
        if (topLevel != m_topLevelTasks.end()) {
            *topLevel = task;
        }
        m_model.updateTask(task);
    } else {
        updateDownloadGroup(task.parentId);
    }
    emit tasksChanged();
}

void TransferManager::updateDownloadGroup(const QString& groupId)
{
    const auto group = m_downloadGroups.value(groupId);
    if (!group) {
        return;
    }
    const auto summary = std::ranges::find_if(m_topLevelTasks, [&groupId](const TransferTask& task) {
        return task.id == groupId;
    });
    if (summary == m_topLevelTasks.end()) {
        return;
    }

    qint64 bytesDone = 0;
    qint64 bytesTotal = 0;
    qint64 currentSpeed = 0;
    auto completeSize = true;
    auto completeProgress = true;
    auto completedFiles = 0;
    auto finishedFiles = 0;
    auto failedFiles = 0;
    auto canceledFiles = 0;
    auto runningFiles = 0;
    auto cancellable = false;

    for (const auto& task : m_tasks) {
        if (task.parentId != groupId) {
            continue;
        }
        if (!checkedAddBytes(bytesDone, std::max<qint64>(0, task.bytesDone))) {
            bytesDone = std::numeric_limits<qint64>::max();
            completeProgress = false;
        }
        saturatingAddBytes(currentSpeed, task.bytesPerSecond);
        if (!checkedAddBytes(bytesTotal, task.bytesTotal)) {
            completeSize = false;
        }
        if (task.status == statusDone()) {
            ++completedFiles;
            ++finishedFiles;
        } else if (task.status == statusFailed()) {
            ++failedFiles;
            ++finishedFiles;
        } else if (task.status == statusCanceled()) {
            ++canceledFiles;
            ++finishedFiles;
        } else if (task.status == statusRunning()) {
            ++runningFiles;
        }
        cancellable = cancellable || task.cancellable;
    }

    summary->bytesDone = bytesDone;
    const auto byteProgressKnown = completeSize && completeProgress;
    summary->bytesTotal = byteProgressKnown ? bytesTotal : -1;
    summary->bytesRemaining = byteProgressKnown ? std::max<qint64>(0, bytesTotal - bytesDone) : -1;
    summary->bytesPerSecond = currentSpeed;
    summary->averageBytesPerSecond = group->started
        ? averageRate(bytesDone, group->elapsed.elapsed())
        : 0;
    summary->completedFileCount = completedFiles;
    summary->fileCount = static_cast<int>(group->taskIds.size());
    summary->detail = QStringLiteral("%1 / %2 files")
                          .arg(completedFiles)
                          .arg(summary->fileCount);
    summary->cancellable = cancellable;

    if (summary->fileCount == 0) {
        summary->status = statusDone();
        summary->progress = 1.0;
        summary->cancellable = false;
    } else if (finishedFiles == summary->fileCount) {
        summary->status = failedFiles > 0
            ? statusFailed()
            : canceledFiles > 0 ? statusCanceled() : statusDone();
        summary->progress = byteProgressKnown && bytesTotal > 0
            ? std::clamp(static_cast<double>(bytesDone) / static_cast<double>(bytesTotal), 0.0, 1.0)
            : 1.0;
    } else {
        summary->status = runningFiles > 0 || group->started ? statusRunning() : statusQueued();
        summary->progress = byteProgressKnown && bytesTotal > 0
            ? std::clamp(static_cast<double>(bytesDone) / static_cast<double>(bytesTotal), 0.0, 1.0)
            : static_cast<double>(finishedFiles) / static_cast<double>(summary->fileCount);
    }

    m_model.updateTask(*summary);
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
        task.bytesRemaining = task.bytesTotal > 0
            ? std::max<qint64>(0, task.bytesTotal - task.bytesDone)
            : -1;
        task.progress = task.bytesTotal > 0
            ? std::clamp(static_cast<double>(task.bytesDone) / static_cast<double>(task.bytesTotal), 0.0, 1.0)
            : 0.0;

        const auto elapsedMs = active->elapsed.elapsed();
        task.averageBytesPerSecond = averageRate(task.bytesDone, elapsedMs);
        const auto sampleDurationMs = elapsedMs - active->speedSampleElapsedMs;
        if (sampleDurationMs >= speedSampleIntervalMs) {
            const auto sampleBytes = task.bytesDone - active->speedSampleBytes;
            task.bytesPerSecond = sampleBytes > 0
                ? averageRate(sampleBytes, sampleDurationMs)
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
        task.averageBytesPerSecond = averageRate(task.bytesDone, active->elapsed.elapsed());
        task.bytesRemaining = task.bytesTotal >= 0
            ? std::max<qint64>(0, task.bytesTotal - task.bytesDone)
            : -1;
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
