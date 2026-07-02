#pragma once

#include "player/PlayerController.h"

#include <QQuickItem>
#include <QPointer>
#include <QRect>
#include <QUrl>
#include <QWindow>
#include <QtQmlIntegration/qqmlintegration.h>

class MpvVideoItem : public QQuickItem {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(QUrl source READ source WRITE setSource NOTIFY sourceChanged)
    Q_PROPERTY(double startPosition READ startPosition WRITE setStartPosition NOTIFY startPositionChanged)
    Q_PROPERTY(QString httpUsername READ httpUsername WRITE setHttpUsername NOTIFY httpAuthChanged)
    Q_PROPERTY(QString httpPassword READ httpPassword WRITE setHttpPassword NOTIFY httpAuthChanged)
    Q_PROPERTY(bool allowInsecureTls READ allowInsecureTls WRITE setAllowInsecureTls NOTIFY httpAuthChanged)
    Q_PROPERTY(bool paused READ paused NOTIFY playbackStateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY playbackStateChanged)
    Q_PROPERTY(bool buffering READ buffering NOTIFY playbackStateChanged)
    Q_PROPERTY(int bufferingProgress READ bufferingProgress NOTIFY playbackStateChanged)
    Q_PROPERTY(bool seeking READ seeking NOTIFY playbackStateChanged)
    Q_PROPERTY(double position READ position NOTIFY playbackStateChanged)
    Q_PROPERTY(double duration READ duration NOTIFY playbackStateChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(double speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(QString videoResolution READ videoResolution NOTIFY videoInfoChanged)
    Q_PROPERTY(QString videoCodec READ videoCodec NOTIFY videoInfoChanged)
    Q_PROPERTY(QString videoFrameRate READ videoFrameRate NOTIFY videoInfoChanged)
    Q_PROPERTY(QString videoBitrate READ videoBitrate NOTIFY videoInfoChanged)
    Q_PROPERTY(double cacheDurationSeconds READ cacheDurationSeconds NOTIFY cacheStatsChanged)
    Q_PROPERTY(TrackListModel* subtitleTracks READ subtitleTracks CONSTANT)
    Q_PROPERTY(TrackListModel* audioTracks READ audioTracks CONSTANT)

public:
    explicit MpvVideoItem(QQuickItem* parent = nullptr);
    ~MpvVideoItem() override;

    QUrl source() const;
    void setSource(const QUrl& value);
    double startPosition() const;
    void setStartPosition(double value);
    QString httpUsername() const;
    void setHttpUsername(const QString& value);
    QString httpPassword() const;
    void setHttpPassword(const QString& value);
    bool allowInsecureTls() const;
    void setAllowInsecureTls(bool value);
    bool paused() const;
    bool loading() const;
    bool buffering() const;
    int bufferingProgress() const;
    bool seeking() const;
    double position() const;
    double duration() const;
    int volume() const;
    double speed() const;
    QString videoResolution() const;
    QString videoCodec() const;
    QString videoFrameRate() const;
    QString videoBitrate() const;
    double cacheDurationSeconds() const;
    TrackListModel* subtitleTracks();
    TrackListModel* audioTracks();

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void resume();
    Q_INVOKABLE void togglePause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seekRelative(double seconds);
    Q_INVOKABLE void seekAbsolute(double seconds);
    Q_INVOKABLE void setVolume(int volume);
    Q_INVOKABLE void setSpeed(double speed);
    Q_INVOKABLE void selectSubtitleTrack(int row);
    Q_INVOKABLE void selectAudioTrack(int row);

signals:
    void sourceChanged();
    void startPositionChanged();
    void httpAuthChanged();
    void errorOccurred(const QString& message);
    void playbackStateChanged();
    void volumeChanged();
    void speedChanged();
    void videoInfoChanged();
    void cacheStatsChanged();
    void tracksChanged();
    void nativeWindowUpdated();
    void playbackRestarted();
    void playbackNetworkBytes(qint64 bytesReceived);

protected:
    void itemChange(ItemChange change, const ItemChangeData& value) override;
    void geometryChange(const QRectF& newGeometry, const QRectF& oldGeometry) override;

private:
    void ensureNativeWindow();
    void destroyNativeWindow();
    void hideNativeWindow();
    void syncWindowGeometry();
    void refreshNativeWindow();
    void scheduleNativeWindowRefresh();
    QString sourceString() const;

    PlayerController m_controller;
    QPointer<QWindow> m_nativeWindow;
    QRect m_lastNativeGeometry;
    quintptr m_nativeWindowId { 0 };
    QUrl m_source;
    QString m_httpUsername;
    QString m_httpPassword;
    bool m_allowInsecureTls { false };
    double m_startPosition { 0.0 };
    qreal m_lastNativeDevicePixelRatio { 0.0 };
    bool m_initialized { false };
    bool m_pendingPlay { false };
};
