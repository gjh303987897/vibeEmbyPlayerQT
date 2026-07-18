#include "services/scheduler/ScheduledPlaybackManager.h"

#include "services/scheduler/ScheduledPlaybackSchedule.h"
#include "utils/AppLogger.h"

#include <QDateTime>
#include <QStringList>
#include <QTimer>
#include <QTimeZone>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr int progressUpdateIntervalMs = 1'000;
constexpr int scheduleCheckIntervalMs = 30'000;
constexpr int scheduleActivationGraceSeconds = 90;
constexpr double progressReportIntervalSeconds = 15.0;
constexpr int randomCandidateCount = 12;
constexpr int maxConsecutiveFailures = 3;
constexpr qint64 ticksPerSecond = 10'000'000;

QString displayNetworkError(const NetworkError& error)
{
    if (!error.message.isEmpty()) {
        return error.message;
    }
    if (error.httpStatus > 0) {
        return QStringLiteral("Server returned HTTP %1").arg(error.httpStatus);
    }
    return QStringLiteral("Scheduled playback network request failed");
}
}

ScheduledPlaybackManager::ScheduledPlaybackManager(EmbyClient& embyClient,
                                                   SessionRepository& repository,
                                                   QObject* parent)
    : QObject(parent)
    , m_embyClient(embyClient)
    , m_repository(repository)
{
    m_progressTimer.setInterval(progressUpdateIntervalMs);
    connect(&m_progressTimer, &QTimer::timeout, this, &ScheduledPlaybackManager::updateProgress);

    m_scheduleTimer.setInterval(scheduleCheckIntervalMs);
    connect(&m_scheduleTimer, &QTimer::timeout, this, &ScheduledPlaybackManager::evaluateScheduledTasks);
    m_scheduleTimer.start();

    connect(&m_player, &PlayerController::playbackRestarted, this, &ScheduledPlaybackManager::handlePlaybackRestarted);
    connect(&m_player, &PlayerController::playbackEnded, this, &ScheduledPlaybackManager::handlePlaybackEnded);
    connect(&m_player, &PlayerController::playbackNetworkBytes, this, [this](qint64 bytesReceived) {
        if (bytesReceived > 0 && m_currentSession) {
            emit networkTrafficSample(m_currentSession->server, bytesReceived);
        }
    });
    connect(&m_player, &PlayerController::errorOccurred, this, [](const QString& message) {
        AppLogger::warning(QStringLiteral("scheduled-playback"), message);
    });
}

void ScheduledPlaybackManager::setTasks(std::vector<ScheduledPlaybackTask> tasks, bool privacyMode)
{
    QHash<QString, ScheduledPlaybackTask> availableTasks;
    for (const auto& task : tasks) {
        availableTasks.insert(task.id, task);
    }

    std::deque<ScheduledPlaybackTask> retainedQueue;
    for (const auto& queuedTask : m_scheduledQueue) {
        const auto updated = availableTasks.constFind(queuedTask.id);
        if (updated != availableTasks.cend() && ScheduledPlaybackSchedule::isAutomatic(*updated)) {
            retainedQueue.push_back(*updated);
        }
    }

    m_privacyMode = privacyMode;
    m_scheduledTasks = std::move(tasks);
    m_scheduledQueue = std::move(retainedQueue);
    checkStartupMissedTasks();
    m_tasksInitialized = true;

    bool missedTasksUpdated = false;
    for (const auto& task : m_scheduledTasks) {
        if (!ScheduledPlaybackSchedule::isAutomatic(task) && m_missedTaskIds.remove(task.id)) {
            missedTasksUpdated = true;
        }
    }
    if (missedTasksUpdated) {
        persistMissedTaskIds();
    }

    rebuildNextRuns();
    startNextScheduledTask();
    emit missedTasksChanged();
}

void ScheduledPlaybackManager::setForegroundPlaybackActive(bool active)
{
    if (m_foregroundPlaybackActive == active) {
        return;
    }
    m_foregroundPlaybackActive = active;

    if (active && m_currentTask) {
        stopCurrentItem(true);
        m_resumeAfterForeground = true;
        setStatus(QStringLiteral("waiting"));
        AppLogger::info(QStringLiteral("scheduled-playback"),
                        QStringLiteral("Background playback yielded to foreground playback"));
        return;
    }

    if (!active) {
        if (m_resumeAfterForeground && m_currentTask) {
            resumeTaskAfterForeground();
        } else if (m_pendingTask) {
            auto task = std::move(*m_pendingTask);
            m_pendingTask.reset();
            startTask(std::move(task));
        }
    }
}

void ScheduledPlaybackManager::runNow(const ScheduledPlaybackTask& task)
{
    if (m_currentTask || m_pendingTask) {
        AppLogger::warning(QStringLiteral("scheduled-playback"),
                           QStringLiteral("Ignored run request while another scheduled task is active"));
        return;
    }
    startTask(task);
}

void ScheduledPlaybackManager::resolveMissedTasks(bool runNow)
{
    std::vector<ScheduledPlaybackTask> resolvedTasks;
    for (const auto& task : m_scheduledTasks) {
        if (m_missedTaskIds.contains(task.id)) {
            resolvedTasks.push_back(task);
        }
    }

    for (const auto& task : resolvedTasks) {
        m_missedTaskIds.remove(task.id);
        if (runNow && ScheduledPlaybackSchedule::isAutomatic(task)) {
            enqueueScheduledTask(task);
        }
    }

    persistMissedTaskIds();
    emit missedTasksChanged();
    if (runNow) {
        startNextScheduledTask();
    }
}

void ScheduledPlaybackManager::stop()
{
    ++m_generation;
    stopCurrentItem(false);
    m_pendingTask.reset();
    m_scheduledQueue.clear();
    m_resumeAfterForeground = false;
    clearRunState();
    setStatus(QStringLiteral("idle"));
    AppLogger::info(QStringLiteral("scheduled-playback"), QStringLiteral("Scheduled playback stopped"));
}

void ScheduledPlaybackManager::rebuildNextRuns()
{
    m_nextRuns.clear();
    m_scheduleTimeZoneId = QTimeZone::systemTimeZoneId();

    const auto now = QDateTime::currentDateTime();
    const auto recentSearchFrom = now.addSecs(-scheduleActivationGraceSeconds);
    for (const auto& task : m_scheduledTasks) {
        const auto& searchFrom = m_missedTaskIds.contains(task.id) ? now : recentSearchFrom;
        if (const auto nextRun = ScheduledPlaybackSchedule::nextOccurrence(task, searchFrom)) {
            m_nextRuns.insert(task.id, *nextRun);
        }
    }
}

void ScheduledPlaybackManager::checkStartupMissedTasks()
{
    auto& checkCompleted = m_privacyMode
        ? m_privateStartupMissedCheckCompleted
        : m_normalStartupMissedCheckCompleted;
    if (checkCompleted) {
        return;
    }
    checkCompleted = true;

    if (!m_pendingMissedTaskIdsLoaded) {
        for (const auto& taskId : m_repository.pendingMissedScheduledPlaybackTaskIds()) {
            if (!taskId.isEmpty()) {
                m_missedTaskIds.insert(taskId);
            }
        }
        m_pendingMissedTaskIdsLoaded = true;
    }

    const auto now = QDateTime::currentDateTime();
    const auto checkpointText = m_repository.scheduledPlaybackCheckpoint(m_privacyMode);
    auto checkpoint = QDateTime::fromString(checkpointText, Qt::ISODateWithMs);
    if (!checkpoint.isValid()) {
        checkpoint = QDateTime::fromString(checkpointText, Qt::ISODate);
    }
    checkpoint = checkpoint.toLocalTime();

    for (const auto& task : m_scheduledTasks) {
        if (task.privateMode != m_privacyMode || !ScheduledPlaybackSchedule::isAutomatic(task)) {
            continue;
        }

        auto taskCheckpoint = checkpoint;
        auto createdAt = QDateTime::fromString(task.createdAt, Qt::ISODateWithMs);
        if (!createdAt.isValid()) {
            createdAt = QDateTime::fromString(task.createdAt, Qt::ISODate);
        }
        createdAt = createdAt.toLocalTime();
        if (createdAt.isValid() && (!taskCheckpoint.isValid() || createdAt > taskCheckpoint)) {
            taskCheckpoint = createdAt;
        }

        if (taskCheckpoint.isValid() && taskCheckpoint < now &&
            ScheduledPlaybackSchedule::firstOccurrenceBetween(task, taskCheckpoint, now)) {
            m_missedTaskIds.insert(task.id);
        }
    }

    if (m_privacyMode) {
        QSet<QString> existingTaskIds;
        for (const auto& task : m_scheduledTasks) {
            existingTaskIds.insert(task.id);
        }
        m_missedTaskIds.intersect(existingTaskIds);
    }

    persistMissedTaskIds();
    m_repository.setScheduledPlaybackCheckpoint(
        m_privacyMode,
        now.toUTC().toString(Qt::ISODateWithMs));
}

void ScheduledPlaybackManager::updateScheduleCheckpoints(const QDateTime& timestamp)
{
    const auto value = timestamp.toUTC().toString(Qt::ISODateWithMs);
    m_repository.setScheduledPlaybackCheckpoint(false, value);
    if (m_privacyMode) {
        m_repository.setScheduledPlaybackCheckpoint(true, value);
    }
}

void ScheduledPlaybackManager::persistMissedTaskIds()
{
    auto taskIds = m_missedTaskIds.values();
    std::ranges::sort(taskIds);
    m_repository.setPendingMissedScheduledPlaybackTaskIds(taskIds);
}

void ScheduledPlaybackManager::evaluateScheduledTasks()
{
    if (!m_tasksInitialized) {
        return;
    }
    if (m_scheduleTimeZoneId != QTimeZone::systemTimeZoneId()) {
        rebuildNextRuns();
    }

    const auto now = QDateTime::currentDateTime();
    QStringList dueTaskIds;
    for (auto it = m_nextRuns.cbegin(); it != m_nextRuns.cend(); ++it) {
        if (it.value() <= now) {
            dueTaskIds.push_back(it.key());
        }
    }

    bool scheduleStateUpdated = false;
    bool checkpointSafe = true;
    for (const auto& taskId : dueTaskIds) {
        const auto taskIt = std::ranges::find(m_scheduledTasks, taskId, &ScheduledPlaybackTask::id);
        if (taskIt == m_scheduledTasks.end() || !ScheduledPlaybackSchedule::isAutomatic(*taskIt)) {
            m_nextRuns.remove(taskId);
            continue;
        }

        const auto runAt = m_nextRuns.value(taskId);
        const auto runDate = runAt.date().toString(Qt::ISODate);
        taskIt->lastRunDate = runDate;
        if (auto result = m_repository.setScheduledPlaybackTaskLastRun(taskIt->id, runDate); !result) {
            AppLogger::warning(QStringLiteral("scheduled-playback"),
                               QStringLiteral("Unable to persist scheduled task run date: %1").arg(result.error()));
            checkpointSafe = false;
        } else {
            scheduleStateUpdated = true;
        }

        enqueueScheduledTask(*taskIt);
        if (const auto nextRun = ScheduledPlaybackSchedule::nextOccurrence(*taskIt, now)) {
            m_nextRuns.insert(taskIt->id, *nextRun);
        } else {
            m_nextRuns.remove(taskIt->id);
        }
    }

    if (scheduleStateUpdated) {
        emit scheduleStateChanged();
    }
    if (checkpointSafe) {
        updateScheduleCheckpoints(now);
    }
    startNextScheduledTask();
}

void ScheduledPlaybackManager::enqueueScheduledTask(const ScheduledPlaybackTask& task)
{
    const auto alreadyQueued = std::ranges::any_of(m_scheduledQueue, [&task](const auto& queued) {
        return queued.id == task.id;
    });
    if (alreadyQueued || (m_currentTask && m_currentTask->id == task.id) ||
        (m_pendingTask && m_pendingTask->id == task.id)) {
        return;
    }

    m_scheduledQueue.push_back(task);
    AppLogger::info(QStringLiteral("scheduled-playback"),
                    QStringLiteral("Automatic keep-alive task queued for source %1").arg(task.serverName));
}

void ScheduledPlaybackManager::startNextScheduledTask()
{
    if (m_currentTask || m_pendingTask || m_scheduledQueue.empty()) {
        return;
    }

    auto task = std::move(m_scheduledQueue.front());
    m_scheduledQueue.pop_front();
    startTask(std::move(task));
}

QString ScheduledPlaybackManager::status() const
{
    return m_status;
}

QString ScheduledPlaybackManager::currentServerName() const
{
    if (m_currentTask) {
        return m_currentTask->serverName;
    }
    if (m_pendingTask) {
        return m_pendingTask->serverName;
    }
    return {};
}

QString ScheduledPlaybackManager::currentMediaName() const
{
    return m_currentItem ? m_currentItem->name : QString {};
}

QString ScheduledPlaybackManager::errorMessage() const
{
    return m_errorMessage;
}

qint64 ScheduledPlaybackManager::elapsedSeconds() const
{
    return currentElapsedSeconds();
}

qint64 ScheduledPlaybackManager::targetSeconds() const
{
    return m_targetSeconds;
}

bool ScheduledPlaybackManager::active() const
{
    return m_status == QStringLiteral("starting") || m_status == QStringLiteral("playing");
}

bool ScheduledPlaybackManager::waiting() const
{
    return m_status == QStringLiteral("waiting");
}

int ScheduledPlaybackManager::missedTaskCount() const
{
    return static_cast<int>(std::ranges::count_if(m_scheduledTasks, [this](const auto& task) {
        return m_missedTaskIds.contains(task.id);
    }));
}

QStringList ScheduledPlaybackManager::missedTaskServerNames() const
{
    QStringList names;
    for (const auto& task : m_scheduledTasks) {
        if (m_missedTaskIds.contains(task.id) && !names.contains(task.serverName)) {
            names.push_back(task.serverName);
        }
    }
    return names;
}

void ScheduledPlaybackManager::startTask(ScheduledPlaybackTask task)
{
    if (m_foregroundPlaybackActive) {
        m_pendingTask = std::move(task);
        setStatus(QStringLiteral("waiting"));
        return;
    }

    const auto sessionResult = m_repository.loadSession(task.serverId);
    if (!sessionResult) {
        failTask(sessionResult.error());
        return;
    }
    if (!sessionResult->has_value()) {
        failTask(QStringLiteral("The selected Emby source has no saved session"));
        return;
    }

    m_currentTask = std::move(task);
    m_currentSession = std::move(sessionResult->value());
    m_completedSeconds = 0;
    m_targetSeconds = std::max(1, m_currentTask->durationMinutes) * 60LL;
    m_consecutiveFailures = 0;
    m_recentItemIds.clear();
    m_resumeAfterForeground = false;
    m_errorMessage.clear();

    setStatus(QStringLiteral("starting"));
    m_progressTimer.start();
    AppLogger::info(QStringLiteral("scheduled-playback"),
                    QStringLiteral("Starting scheduled playback for source %1").arg(m_currentTask->serverName));
    fetchCandidates();
}

void ScheduledPlaybackManager::resumeTaskAfterForeground()
{
    m_resumeAfterForeground = false;
    if (currentElapsedSeconds() >= m_targetSeconds) {
        finishTask();
        return;
    }
    setStatus(QStringLiteral("starting"));
    m_progressTimer.start();
    fetchCandidates();
}

void ScheduledPlaybackManager::fetchCandidates()
{
    if (!m_currentSession || !m_currentTask) {
        failTask(QStringLiteral("Scheduled playback session is unavailable"));
        return;
    }

    const auto generation = ++m_generation;
    m_embyClient.fetchRandomPlayableItems(*m_currentSession,
                                          randomCandidateCount,
                                          [this, generation](ItemResult result) {
        if (generation != m_generation || !m_currentTask) {
            return;
        }
        if (!result) {
            failTask(displayNetworkError(result.error()));
            return;
        }

        m_candidates.clear();
        for (auto& item : *result) {
            if (item.id.isEmpty() || m_recentItemIds.contains(item.id)) {
                continue;
            }
            m_candidates.push_back(std::move(item));
        }
        if (m_candidates.empty()) {
            m_recentItemIds.clear();
            m_candidates = std::move(*result);
        }
        tryNextCandidate();
    });
}

void ScheduledPlaybackManager::tryNextCandidate()
{
    if (!m_currentTask || !m_currentSession) {
        return;
    }
    if (m_foregroundPlaybackActive) {
        m_resumeAfterForeground = true;
        setStatus(QStringLiteral("waiting"));
        return;
    }
    if (m_candidates.empty()) {
        fetchCandidates();
        return;
    }

    auto item = std::move(m_candidates.back());
    m_candidates.pop_back();
    if (item.id.isEmpty()) {
        tryNextCandidate();
        return;
    }

    const auto generation = m_generation;
    m_embyClient.fetchPlaybackUrl(*m_currentSession,
                                  item,
                                  [this, generation, item](PlaybackUrlResult result) mutable {
        if (generation != m_generation || !m_currentTask) {
            return;
        }
        if (!result) {
            ++m_consecutiveFailures;
            if (m_consecutiveFailures >= maxConsecutiveFailures) {
                failTask(displayNetworkError(result.error()));
                return;
            }
            tryNextCandidate();
            return;
        }
        beginPlayback(std::move(item), std::move(*result));
    });
}

void ScheduledPlaybackManager::beginPlayback(MediaItem item, PlaybackRequest request)
{
    if (m_foregroundPlaybackActive) {
        m_resumeAfterForeground = true;
        setStatus(QStringLiteral("waiting"));
        return;
    }
    if (!m_player.initializeHeadless()) {
        failTask(QStringLiteral("Unable to initialize background playback"));
        return;
    }

    m_recentItemIds.insert(item.id);
    m_currentItem = std::move(item);
    m_currentRequest = std::move(request);
    m_currentItemStarted = false;
    m_lastReportedPosition = -1.0;
    m_ignoreNextPlaybackEnd = false;
    setStatus(QStringLiteral("starting"));
    m_player.playUrl(m_currentRequest->url.toString(QUrl::FullyEncoded));
}

void ScheduledPlaybackManager::handlePlaybackRestarted()
{
    if (!m_currentItem || !m_currentRequest || m_currentItemStarted) {
        return;
    }
    m_currentItemStarted = true;
    m_consecutiveFailures = 0;
    reportCurrentStart();
    setStatus(QStringLiteral("playing"));
    AppLogger::info(QStringLiteral("scheduled-playback"),
                    QStringLiteral("Background media playback started: %1").arg(m_currentItem->name));
}

void ScheduledPlaybackManager::handlePlaybackEnded(double positionSeconds, bool reachedEnd, bool failed)
{
    Q_UNUSED(reachedEnd)
    if (m_ignoreNextPlaybackEnd) {
        m_ignoreNextPlaybackEnd = false;
        return;
    }
    if (!m_currentItem || !m_currentTask) {
        return;
    }

    const auto playedSeconds = std::max(0.0, positionSeconds);
    if (m_currentItemStarted) {
        reportCurrentStop(playedSeconds);
    }
    m_completedSeconds += static_cast<qint64>(std::floor(playedSeconds));
    m_currentItem.reset();
    m_currentRequest.reset();
    m_currentItemStarted = false;
    m_lastReportedPosition = -1.0;
    emit statusChanged();

    if (m_completedSeconds >= m_targetSeconds) {
        finishTask();
        return;
    }
    if (failed) {
        ++m_consecutiveFailures;
        if (m_consecutiveFailures >= maxConsecutiveFailures) {
            failTask(QStringLiteral("Background playback failed repeatedly"));
            return;
        }
    } else {
        m_consecutiveFailures = 0;
    }
    tryNextCandidate();
}

void ScheduledPlaybackManager::updateProgress()
{
    if (!m_currentTask) {
        return;
    }
    const auto elapsed = currentElapsedSeconds();
    if (elapsed >= m_targetSeconds) {
        finishTask();
        return;
    }

    if (m_currentItemStarted) {
        const auto position = std::max(0.0, m_player.position());
        if (m_lastReportedPosition < 0.0 || position - m_lastReportedPosition >= progressReportIntervalSeconds) {
            reportCurrentProgress(position);
            m_lastReportedPosition = position;
        }
    }
    emit statusChanged();
}

void ScheduledPlaybackManager::stopCurrentItem(bool accumulatePosition)
{
    if (!m_currentItem) {
        return;
    }
    const auto position = std::max(0.0, m_player.position());
    if (m_currentItemStarted) {
        reportCurrentStop(position);
    }
    if (accumulatePosition) {
        m_completedSeconds += static_cast<qint64>(std::floor(position));
    }
    m_ignoreNextPlaybackEnd = true;
    m_player.stop();
    m_currentItem.reset();
    m_currentRequest.reset();
    m_currentItemStarted = false;
    m_lastReportedPosition = -1.0;
}

void ScheduledPlaybackManager::reportCurrentStart()
{
    if (!m_currentSession || !m_currentItem || !m_currentRequest) {
        return;
    }
    m_embyClient.reportPlaybackStart(*m_currentSession, PlaybackReport {
        .itemId = m_currentItem->id,
        .mediaSourceId = m_currentRequest->mediaSourceId,
        .playSessionId = m_currentRequest->playSessionId,
        .positionTicks = 0,
        .paused = false,
    });
}

void ScheduledPlaybackManager::reportCurrentProgress(double positionSeconds)
{
    if (!m_currentSession || !m_currentItem || !m_currentRequest) {
        return;
    }
    m_embyClient.reportPlaybackProgress(*m_currentSession, PlaybackReport {
        .itemId = m_currentItem->id,
        .mediaSourceId = m_currentRequest->mediaSourceId,
        .playSessionId = m_currentRequest->playSessionId,
        .positionTicks = static_cast<qint64>(std::max(0.0, positionSeconds) * ticksPerSecond),
        .paused = false,
    });
}

void ScheduledPlaybackManager::reportCurrentStop(double positionSeconds)
{
    if (!m_currentSession || !m_currentItem || !m_currentRequest) {
        return;
    }
    m_embyClient.reportPlaybackStopped(*m_currentSession, PlaybackReport {
        .itemId = m_currentItem->id,
        .mediaSourceId = m_currentRequest->mediaSourceId,
        .playSessionId = m_currentRequest->playSessionId,
        .positionTicks = static_cast<qint64>(std::max(0.0, positionSeconds) * ticksPerSecond),
        .paused = false,
    });
}

void ScheduledPlaybackManager::finishTask()
{
    ++m_generation;
    stopCurrentItem(false);
    m_progressTimer.stop();
    m_completedSeconds = m_targetSeconds;
    m_currentSession.reset();
    m_currentTask.reset();
    m_resumeAfterForeground = false;
    setStatus(QStringLiteral("completed"));
    AppLogger::info(QStringLiteral("scheduled-playback"), QStringLiteral("Scheduled playback duration completed"));
    QTimer::singleShot(0, this, &ScheduledPlaybackManager::startNextScheduledTask);
}

void ScheduledPlaybackManager::failTask(QString message)
{
    ++m_generation;
    stopCurrentItem(false);
    m_progressTimer.stop();
    m_currentSession.reset();
    m_currentTask.reset();
    m_resumeAfterForeground = false;
    setStatus(QStringLiteral("error"), std::move(message));
    AppLogger::warning(QStringLiteral("scheduled-playback"), m_errorMessage);
    QTimer::singleShot(0, this, &ScheduledPlaybackManager::startNextScheduledTask);
}

void ScheduledPlaybackManager::setStatus(QString status, QString error)
{
    m_status = std::move(status);
    m_errorMessage = std::move(error);
    emit statusChanged();
}

void ScheduledPlaybackManager::clearRunState()
{
    m_progressTimer.stop();
    m_candidates.clear();
    m_recentItemIds.clear();
    m_currentTask.reset();
    m_currentSession.reset();
    m_currentItem.reset();
    m_currentRequest.reset();
    m_completedSeconds = 0;
    m_targetSeconds = 0;
    m_lastReportedPosition = -1.0;
    m_currentItemStarted = false;
    m_consecutiveFailures = 0;
}

qint64 ScheduledPlaybackManager::currentElapsedSeconds() const
{
    const auto currentPosition = m_currentItemStarted ? std::max(0.0, m_player.position()) : 0.0;
    return std::min(m_targetSeconds,
                    m_completedSeconds + static_cast<qint64>(std::floor(currentPosition)));
}
