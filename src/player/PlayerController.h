#pragma once

#include <QAbstractListModel>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QtGlobal>
#include <vector>

struct mpv_handle;

struct TrackInfo {
    int id { -1 };
    QString type;
    QString title;
    QString language;
    QString codec;
    bool selected { false };
};

class TrackListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        TypeRole,
        TitleRole,
        LanguageRole,
        CodecRole,
        SelectedRole,
        DisplayNameRole,
    };

    explicit TrackListModel(QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int count() const;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setTracks(std::vector<TrackInfo> tracks);
    const TrackInfo* trackAt(int row) const;

signals:
    void countChanged();

private:
    std::vector<TrackInfo> m_tracks;
};

class PlayerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool paused READ paused NOTIFY playbackStateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY playbackStateChanged)
    Q_PROPERTY(bool buffering READ buffering NOTIFY playbackStateChanged)
    Q_PROPERTY(int bufferingProgress READ bufferingProgress NOTIFY playbackStateChanged)
    Q_PROPERTY(bool seeking READ seeking NOTIFY playbackStateChanged)
    Q_PROPERTY(double position READ position NOTIFY playbackStateChanged)
    Q_PROPERTY(double duration READ duration NOTIFY playbackStateChanged)
    Q_PROPERTY(int volume READ volume NOTIFY volumeChanged)
    Q_PROPERTY(double speed READ speed NOTIFY speedChanged)
    Q_PROPERTY(QString videoResolution READ videoResolution NOTIFY videoInfoChanged)
    Q_PROPERTY(QString videoCodec READ videoCodec NOTIFY videoInfoChanged)
    Q_PROPERTY(QString videoFrameRate READ videoFrameRate NOTIFY videoInfoChanged)
    Q_PROPERTY(QString videoBitrate READ videoBitrate NOTIFY videoInfoChanged)
    Q_PROPERTY(double cacheDurationSeconds READ cacheDurationSeconds NOTIFY cacheStatsChanged)
    Q_PROPERTY(TrackListModel* subtitleTracks READ subtitleTracks CONSTANT)
    Q_PROPERTY(TrackListModel* audioTracks READ audioTracks CONSTANT)

public:
    explicit PlayerController(QObject* parent = nullptr);
    ~PlayerController() override;

    bool initialize(qintptr windowId);
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

    Q_INVOKABLE void playUrl(const QString& url,
                             double startSeconds = 0.0,
                             const QString& httpUsername = {},
                             const QString& httpPassword = {},
                             bool allowInsecureTls = false);
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
    void shutdown();

signals:
    void errorOccurred(const QString& message);
    void playbackStateChanged();
    void volumeChanged();
    void speedChanged();
    void videoInfoChanged();
    void cacheStatsChanged();
    void tracksChanged();
    void videoOutputChanged();
    void playbackRestarted();

private:
    void observeProperties();
    void processEvents();
    void handlePropertyChange(const char* name, int format, void* data);
    void updateTracks();
    void updateVideoInfo(QString resolution, QString codec, QString frameRate, QString bitrate);
    void updateCacheDuration(double seconds);
    void resetPlaybackState();
    static QString nodeString(const struct mpv_node& node);
    static int nodeInt(const struct mpv_node& node, int fallback = -1);
    static qint64 nodeInt64(const struct mpv_node& node, qint64 fallback = 0);
    static double nodeDouble(const struct mpv_node& node, double fallback = 0.0);
    static bool nodeBool(const struct mpv_node& node, bool fallback = false);
    bool command(const char** args);

    mpv_handle* m_mpv { nullptr };
    qintptr m_windowId { 0 };
    bool m_paused { false };
    bool m_loading { false };
    bool m_buffering { false };
    bool m_seeking { false };
    int m_bufferingProgress { 0 };
    double m_position { 0.0 };
    double m_duration { 0.0 };
    int m_volume { 100 };
    double m_speed { 1.0 };
    QString m_videoResolution;
    QString m_videoCodec;
    QString m_videoFrameRate;
    QString m_videoBitrate;
    double m_cacheDurationSeconds { -1.0 };
    QTimer m_eventTimer;
    TrackListModel m_subtitleTracks;
    TrackListModel m_audioTracks;
};
