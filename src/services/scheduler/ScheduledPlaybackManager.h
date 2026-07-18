#pragma once

#include "database/SessionRepository.h"
#include "models/MediaItem.h"
#include "models/ScheduledPlaybackTask.h"
#include "models/UserSession.h"
#include "player/PlayerController.h"
#include "services/emby/EmbyClient.h"
#include "services/media/MediaServiceClient.h"

#include <QObject>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <deque>
#include <optional>
#include <vector>

class ScheduledPlaybackManager final : public QObject {
    Q_OBJECT

public:
    explicit ScheduledPlaybackManager(EmbyClient& embyClient,
                                      SessionRepository& repository,
                                      QObject* parent = nullptr);

    void setTasks(std::vector<ScheduledPlaybackTask> tasks, bool privacyMode);
    void setForegroundPlaybackActive(bool active);
    void runNow(const ScheduledPlaybackTask& task);
    void resolveMissedTasks(bool runNow);
    void stop();

    QString status() const;
    QString currentServerName() const;
    QString currentMediaName() const;
    QString errorMessage() const;
    qint64 elapsedSeconds() const;
    qint64 targetSeconds() const;
    bool active() const;
    bool waiting() const;
    int missedTaskCount() const;
    QStringList missedTaskServerNames() const;

signals:
    void statusChanged();
    void scheduleStateChanged();
    void missedTasksChanged();
    void networkTrafficSample(const ServerConfig& server, qint64 bytesReceived);

private:
    void rebuildNextRuns();
    void checkStartupMissedTasks();
    void updateScheduleCheckpoints(const QDateTime& timestamp);
    void persistMissedTaskIds();
    void evaluateScheduledTasks();
    void enqueueScheduledTask(const ScheduledPlaybackTask& task);
    void startNextScheduledTask();
    void startTask(ScheduledPlaybackTask task);
    void resumeTaskAfterForeground();
    void fetchCandidates();
    void tryNextCandidate();
    void beginPlayback(MediaItem item, PlaybackRequest request);
    void handlePlaybackRestarted();
    void handlePlaybackEnded(double positionSeconds, bool reachedEnd, bool failed);
    void updateProgress();
    void stopCurrentItem(bool accumulatePosition);
    void reportCurrentStart();
    void reportCurrentProgress(double positionSeconds);
    void reportCurrentStop(double positionSeconds);
    void finishTask();
    void failTask(QString message);
    void setStatus(QString status, QString error = {});
    void clearRunState();
    qint64 currentElapsedSeconds() const;

    EmbyClient& m_embyClient;
    SessionRepository& m_repository;
    PlayerController m_player;
    QTimer m_progressTimer;
    QTimer m_scheduleTimer;
    std::vector<MediaItem> m_candidates;
    std::vector<ScheduledPlaybackTask> m_scheduledTasks;
    std::deque<ScheduledPlaybackTask> m_scheduledQueue;
    QHash<QString, QDateTime> m_nextRuns;
    QByteArray m_scheduleTimeZoneId;
    QSet<QString> m_missedTaskIds;
    QSet<QString> m_recentItemIds;
    std::optional<ScheduledPlaybackTask> m_currentTask;
    std::optional<ScheduledPlaybackTask> m_pendingTask;
    std::optional<UserSession> m_currentSession;
    std::optional<MediaItem> m_currentItem;
    std::optional<PlaybackRequest> m_currentRequest;
    QString m_status { QStringLiteral("idle") };
    QString m_errorMessage;
    qint64 m_completedSeconds { 0 };
    qint64 m_targetSeconds { 0 };
    double m_lastReportedPosition { -1.0 };
    bool m_foregroundPlaybackActive { false };
    bool m_privacyMode { false };
    bool m_tasksInitialized { false };
    bool m_pendingMissedTaskIdsLoaded { false };
    bool m_normalStartupMissedCheckCompleted { false };
    bool m_privateStartupMissedCheckCompleted { false };
    bool m_currentItemStarted { false };
    bool m_resumeAfterForeground { false };
    bool m_ignoreNextPlaybackEnd { false };
    int m_consecutiveFailures { 0 };
    int m_generation { 0 };
};
