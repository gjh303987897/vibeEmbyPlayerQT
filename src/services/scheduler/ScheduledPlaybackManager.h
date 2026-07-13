#pragma once

#include "database/SessionRepository.h"
#include "models/MediaItem.h"
#include "models/ScheduledPlaybackTask.h"
#include "models/UserSession.h"
#include "player/PlayerController.h"
#include "services/emby/EmbyClient.h"
#include "services/media/MediaServiceClient.h"

#include <QObject>
#include <QSet>
#include <QTimer>

#include <optional>
#include <vector>

class ScheduledPlaybackManager final : public QObject {
    Q_OBJECT

public:
    explicit ScheduledPlaybackManager(EmbyClient& embyClient,
                                      SessionRepository& repository,
                                      QObject* parent = nullptr);

    void setForegroundPlaybackActive(bool active);
    void runNow(const ScheduledPlaybackTask& task);
    void stop();

    QString status() const;
    QString currentServerName() const;
    QString currentMediaName() const;
    QString errorMessage() const;
    qint64 elapsedSeconds() const;
    qint64 targetSeconds() const;
    bool active() const;
    bool waiting() const;

signals:
    void statusChanged();
    void networkTrafficSample(const ServerConfig& server, qint64 bytesReceived);

private:
    void startTask(ScheduledPlaybackTask task);
    void resumeTaskAfterForeground();
    void fetchCandidates();
    void tryNextCandidate();
    void beginPlayback(MediaItem item, PlaybackRequest request);
    void handlePlaybackRestarted();
    void handlePlaybackEnded(double positionSeconds, bool failed);
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
    std::vector<MediaItem> m_candidates;
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
    bool m_currentItemStarted { false };
    bool m_resumeAfterForeground { false };
    bool m_ignoreNextPlaybackEnd { false };
    int m_consecutiveFailures { 0 };
    int m_generation { 0 };
};
