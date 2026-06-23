#include "player/PlayerController.h"

#include "utils/AppLogger.h"

#include <mpv/client.h>

#include <algorithm>
#include <QVariant>
#include <QStringList>
#include <cmath>
#include <utility>

namespace {
constexpr uint64_t propertyTimePos = 1;
constexpr uint64_t propertyDuration = 2;
constexpr uint64_t propertyPause = 3;
constexpr uint64_t propertyVolume = 4;
constexpr uint64_t propertySpeed = 5;
constexpr uint64_t propertyTrackList = 6;
constexpr uint64_t propertyPausedForCache = 7;
constexpr uint64_t propertyCacheBufferingState = 8;
constexpr uint64_t propertySeeking = 9;
constexpr uint64_t propertyIdleActive = 10;
constexpr uint64_t propertyDemuxerCacheDuration = 11;

QString endFileReasonName(mpv_end_file_reason reason)
{
    switch (reason) {
    case MPV_END_FILE_REASON_EOF:
        return QStringLiteral("eof");
    case MPV_END_FILE_REASON_STOP:
        return QStringLiteral("stop");
    case MPV_END_FILE_REASON_QUIT:
        return QStringLiteral("quit");
    case MPV_END_FILE_REASON_ERROR:
        return QStringLiteral("error");
    case MPV_END_FILE_REASON_REDIRECT:
        return QStringLiteral("redirect");
    }
    return QStringLiteral("unknown");
}

QString displayNameFor(const TrackInfo& track)
{
    QStringList parts;
    if (!track.language.isEmpty()) {
        parts.push_back(track.language);
    }
    if (!track.title.isEmpty()) {
        parts.push_back(track.title);
    }
    if (!track.codec.isEmpty()) {
        parts.push_back(track.codec);
    }
    if (parts.isEmpty()) {
        parts.push_back(QStringLiteral("#%1").arg(track.id));
    }
    return parts.join(QStringLiteral(" · "));
}

struct VideoTrackDetails {
    bool available { false };
    bool selected { false };
    QString codec;
    int width { -1 };
    int height { -1 };
    double frameRate { 0.0 };
    qint64 bitrate { 0 };
};

QString trimNumberText(QString value)
{
    while (value.contains(QLatin1Char('.')) && value.endsWith(QLatin1Char('0'))) {
        value.chop(1);
    }
    if (value.endsWith(QLatin1Char('.'))) {
        value.chop(1);
    }
    return value;
}

QString formatVideoResolution(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return {};
    }
    return QStringLiteral("%1 x %2").arg(width).arg(height);
}

QString formatVideoCodec(QString codec)
{
    codec = codec.trimmed();
    if (codec.isEmpty()) {
        return {};
    }
    return codec.toUpper();
}

QString formatFrameRate(double frameRate)
{
    if (!std::isfinite(frameRate) || frameRate <= 0.0) {
        return {};
    }
    return QStringLiteral("%1 fps").arg(trimNumberText(QString::number(frameRate, 'f', 2)));
}

QString formatBitrate(qint64 bitsPerSecond)
{
    if (bitsPerSecond <= 0) {
        return {};
    }
    if (bitsPerSecond >= 1000 * 1000) {
        return QStringLiteral("%1 Mbps").arg(trimNumberText(QString::number(static_cast<double>(bitsPerSecond) / 1000000.0, 'f', 2)));
    }
    if (bitsPerSecond >= 1000) {
        return QStringLiteral("%1 Kbps").arg(trimNumberText(QString::number(static_cast<double>(bitsPerSecond) / 1000.0, 'f', 1)));
    }
    return QStringLiteral("%1 bps").arg(bitsPerSecond);
}
}

TrackListModel::TrackListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

int TrackListModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(m_tracks.size());
}

int TrackListModel::count() const
{
    return rowCount();
}

QVariant TrackListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) {
        return {};
    }

    const auto& track = m_tracks[static_cast<size_t>(index.row())];
    switch (role) {
    case IdRole:
        return track.id;
    case TypeRole:
        return track.type;
    case TitleRole:
        return track.title;
    case LanguageRole:
        return track.language;
    case CodecRole:
        return track.codec;
    case SelectedRole:
        return track.selected;
    case DisplayNameRole:
        return displayNameFor(track);
    default:
        return {};
    }
}

QHash<int, QByteArray> TrackListModel::roleNames() const
{
    return {
        { IdRole, "trackId" },
        { TypeRole, "trackType" },
        { TitleRole, "title" },
        { LanguageRole, "language" },
        { CodecRole, "codec" },
        { SelectedRole, "selected" },
        { DisplayNameRole, "displayName" },
    };
}

void TrackListModel::setTracks(std::vector<TrackInfo> tracks)
{
    beginResetModel();
    m_tracks = std::move(tracks);
    endResetModel();
    emit countChanged();
}

const TrackInfo* TrackListModel::trackAt(int row) const
{
    if (row < 0 || row >= rowCount()) {
        return nullptr;
    }
    return &m_tracks[static_cast<size_t>(row)];
}

PlayerController::PlayerController(QObject* parent)
    : QObject(parent)
{
    m_eventTimer.setInterval(40);
    connect(&m_eventTimer, &QTimer::timeout, this, &PlayerController::processEvents);
}

PlayerController::~PlayerController()
{
    shutdown();
}

bool PlayerController::initialize(qintptr windowId)
{
    if (m_mpv) {
        return true;
    }

    m_windowId = windowId;
    m_mpv = mpv_create();
    if (!m_mpv) {
        emit errorOccurred(QStringLiteral("Unable to create libmpv handle"));
        return false;
    }

    mpv_set_option_string(m_mpv, "terminal", "no");
    mpv_set_option_string(m_mpv, "force-window", "yes");
    mpv_set_option_string(m_mpv, "idle", "yes");
    mpv_set_option_string(m_mpv, "keep-open", "yes");
    mpv_set_option_string(m_mpv, "input-default-bindings", "yes");
    mpv_request_log_messages(m_mpv, "info");

    auto wid = static_cast<int64_t>(m_windowId);
    if (mpv_set_option(m_mpv, "wid", MPV_FORMAT_INT64, &wid) < 0) {
        emit errorOccurred(QStringLiteral("Unable to attach libmpv to native window"));
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    if (mpv_initialize(m_mpv) < 0) {
        emit errorOccurred(QStringLiteral("Unable to initialize libmpv"));
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
        return false;
    }

    observeProperties();
    m_eventTimer.start();
    AppLogger::info(QStringLiteral("player"), QStringLiteral("libmpv initialized with window embedding"));
    return true;
}

bool PlayerController::paused() const
{
    return m_paused;
}

bool PlayerController::loading() const
{
    return m_loading;
}

bool PlayerController::buffering() const
{
    return m_buffering;
}

int PlayerController::bufferingProgress() const
{
    return m_bufferingProgress;
}

bool PlayerController::seeking() const
{
    return m_seeking;
}

double PlayerController::position() const
{
    return m_position;
}

double PlayerController::duration() const
{
    return m_duration;
}

int PlayerController::volume() const
{
    return m_volume;
}

double PlayerController::speed() const
{
    return m_speed;
}

QString PlayerController::videoResolution() const
{
    return m_videoResolution;
}

QString PlayerController::videoCodec() const
{
    return m_videoCodec;
}

QString PlayerController::videoFrameRate() const
{
    return m_videoFrameRate;
}

QString PlayerController::videoBitrate() const
{
    return m_videoBitrate;
}

double PlayerController::cacheDurationSeconds() const
{
    return m_cacheDurationSeconds;
}

TrackListModel* PlayerController::subtitleTracks()
{
    return &m_subtitleTracks;
}

TrackListModel* PlayerController::audioTracks()
{
    return &m_audioTracks;
}

void PlayerController::playUrl(const QString& url,
                               double startSeconds,
                               const QString& httpUsername,
                               const QString& httpPassword,
                               bool allowInsecureTls)
{
    if (!m_mpv || url.isEmpty()) {
        return;
    }
    if (!httpUsername.isEmpty()) {
        const auto username = httpUsername.toUtf8();
        mpv_set_option_string(m_mpv, "http-header-fields", "");
        mpv_set_option_string(m_mpv, "http-user", username.constData());
    } else {
        mpv_set_option_string(m_mpv, "http-user", "");
    }
    if (!httpPassword.isEmpty()) {
        const auto password = httpPassword.toUtf8();
        mpv_set_option_string(m_mpv, "http-password", password.constData());
    } else {
        mpv_set_option_string(m_mpv, "http-password", "");
    }
    mpv_set_option_string(m_mpv, "tls-verify", allowInsecureTls ? "no" : "yes");
    const QByteArray encoded = url.toUtf8();
    updateVideoInfo({}, {}, {}, {});
    updateCacheDuration(-1.0);
    m_loading = true;
    m_buffering = false;
    m_seeking = false;
    m_bufferingProgress = 0;
    emit playbackStateChanged();
    bool requested = false;
    if (startSeconds > 1.0) {
        const QByteArray startOption = QByteArrayLiteral("start=") + QByteArray::number(startSeconds, 'f', 3);
        const char* args[] = { "loadfile", encoded.constData(), "replace", "-1", startOption.constData(), nullptr };
        requested = command(args);
    } else {
        const char* args[] = { "loadfile", encoded.constData(), "replace", nullptr };
        requested = command(args);
    }
    if (!requested) {
        m_loading = false;
        emit playbackStateChanged();
    }
    AppLogger::info(QStringLiteral("player"), QStringLiteral("Playback requested"));
}

void PlayerController::pause()
{
    if (!m_mpv) {
        return;
    }
    int flag = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void PlayerController::resume()
{
    if (!m_mpv) {
        return;
    }
    int flag = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &flag);
}

void PlayerController::togglePause()
{
    if (m_paused) {
        resume();
    } else {
        pause();
    }
}

void PlayerController::stop()
{
    if (!m_mpv) {
        return;
    }
    const char* args[] = { "stop", nullptr };
    command(args);
    resetPlaybackState();
}

void PlayerController::shutdown()
{
    m_eventTimer.stop();
    if (m_mpv) {
        mpv_command_string(m_mpv, "stop");
        mpv_terminate_destroy(m_mpv);
        m_mpv = nullptr;
    }
    resetPlaybackState();
}

void PlayerController::seekRelative(double seconds)
{
    if (!m_mpv) {
        return;
    }
    const QByteArray offset = QByteArray::number(seconds, 'f', 2);
    const char* args[] = { "seek", offset.constData(), "relative", "exact", nullptr };
    command(args);
}

void PlayerController::seekAbsolute(double seconds)
{
    if (!m_mpv) {
        return;
    }
    const auto clamped = std::max(0.0, seconds);
    const QByteArray target = QByteArray::number(clamped, 'f', 2);
    const char* args[] = { "seek", target.constData(), "absolute", "exact", nullptr };
    command(args);
}

void PlayerController::setVolume(int volume)
{
    if (!m_mpv) {
        return;
    }
    double value = static_cast<double>(std::clamp(volume, 0, 100));
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &value);
}

void PlayerController::setSpeed(double speed)
{
    if (!m_mpv) {
        return;
    }
    auto value = std::clamp(speed, 0.25, 4.0);
    mpv_set_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &value);
}

void PlayerController::selectSubtitleTrack(int row)
{
    if (!m_mpv) {
        return;
    }

    if (row < 0) {
        const char* none = "no";
        mpv_set_property_string(m_mpv, "sid", none);
        return;
    }

    const auto* track = m_subtitleTracks.trackAt(row);
    if (!track) {
        return;
    }
    const QByteArray id = QByteArray::number(track->id);
    mpv_set_property_string(m_mpv, "sid", id.constData());
}

void PlayerController::selectAudioTrack(int row)
{
    if (!m_mpv) {
        return;
    }

    const auto* track = m_audioTracks.trackAt(row);
    if (!track) {
        return;
    }
    const QByteArray id = QByteArray::number(track->id);
    mpv_set_property_string(m_mpv, "aid", id.constData());
}

void PlayerController::observeProperties()
{
    if (!m_mpv) {
        return;
    }
    mpv_observe_property(m_mpv, propertyTimePos, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, propertyDuration, "duration", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, propertyPause, "pause", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, propertyVolume, "volume", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, propertySpeed, "speed", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, propertyTrackList, "track-list", MPV_FORMAT_NODE);
    mpv_observe_property(m_mpv, propertyPausedForCache, "paused-for-cache", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, propertyCacheBufferingState, "cache-buffering-state", MPV_FORMAT_INT64);
    mpv_observe_property(m_mpv, propertySeeking, "seeking", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, propertyIdleActive, "idle-active", MPV_FORMAT_FLAG);
    mpv_observe_property(m_mpv, propertyDemuxerCacheDuration, "demuxer-cache-duration", MPV_FORMAT_DOUBLE);
}

void PlayerController::processEvents()
{
    if (!m_mpv) {
        return;
    }

    while (true) {
        auto* event = mpv_wait_event(m_mpv, 0.0);
        if (!event || event->event_id == MPV_EVENT_NONE) {
            break;
        }

        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            const auto* property = static_cast<mpv_event_property*>(event->data);
            if (property) {
                handlePropertyChange(property->name, property->format, property->data);
            }
        } else if (event->event_id == MPV_EVENT_LOG_MESSAGE) {
            const auto* message = static_cast<mpv_event_log_message*>(event->data);
            if (message && message->level && message->prefix && message->text) {
                const auto text = QString::fromUtf8(message->text).trimmed();
                if (!text.isEmpty()) {
                    AppLogger::info(QStringLiteral("mpv"),
                                    QStringLiteral("[%1/%2] %3")
                                        .arg(QString::fromUtf8(message->level),
                                             QString::fromUtf8(message->prefix),
                                             text));
                }
            }
        } else if (event->event_id == MPV_EVENT_END_FILE) {
            const auto* endFile = static_cast<mpv_event_end_file*>(event->data);
            if (endFile) {
                const auto reason = endFileReasonName(endFile->reason);
                if (endFile->reason == MPV_END_FILE_REASON_ERROR) {
                    const auto error = QString::fromUtf8(mpv_error_string(endFile->error));
                    AppLogger::warning(QStringLiteral("player"),
                                       QStringLiteral("libmpv ended file with error: %1").arg(error));
                    emit errorOccurred(QStringLiteral("Playback failed: %1").arg(error));
                } else {
                    AppLogger::info(QStringLiteral("player"),
                                    QStringLiteral("libmpv ended file, reason=%1").arg(reason));
                }
            }
            m_position = 0.0;
            m_loading = false;
            m_buffering = false;
            m_seeking = false;
            m_bufferingProgress = 0;
            updateCacheDuration(-1.0);
            emit playbackStateChanged();
        } else if (event->event_id == MPV_EVENT_FILE_LOADED ||
                   event->event_id == MPV_EVENT_VIDEO_RECONFIG ||
                   event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
            AppLogger::info(QStringLiteral("player"), QStringLiteral("libmpv video output event %1").arg(static_cast<int>(event->event_id)));
            updateTracks();
            if (event->event_id == MPV_EVENT_FILE_LOADED || event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
                m_loading = false;
                emit playbackStateChanged();
            }
            if (event->event_id == MPV_EVENT_PLAYBACK_RESTART) {
                emit playbackRestarted();
            }
            emit videoOutputChanged();
        }
    }
}

void PlayerController::handlePropertyChange(const char* name, int format, void* data)
{
    if (!name) {
        return;
    }

    const auto propertyName = QString::fromLatin1(name);
    if (propertyName == QStringLiteral("demuxer-cache-duration") && format == MPV_FORMAT_NONE) {
        updateCacheDuration(-1.0);
        return;
    }
    if (!data || format == MPV_FORMAT_NONE) {
        return;
    }

    if (propertyName == QStringLiteral("time-pos") && format == MPV_FORMAT_DOUBLE) {
        m_position = *static_cast<double*>(data);
        emit playbackStateChanged();
    } else if (propertyName == QStringLiteral("duration") && format == MPV_FORMAT_DOUBLE) {
        m_duration = *static_cast<double*>(data);
        emit playbackStateChanged();
    } else if (propertyName == QStringLiteral("pause") && format == MPV_FORMAT_FLAG) {
        m_paused = *static_cast<int*>(data) != 0;
        emit playbackStateChanged();
    } else if (propertyName == QStringLiteral("volume") && format == MPV_FORMAT_DOUBLE) {
        m_volume = static_cast<int>(*static_cast<double*>(data));
        emit volumeChanged();
    } else if (propertyName == QStringLiteral("speed") && format == MPV_FORMAT_DOUBLE) {
        m_speed = *static_cast<double*>(data);
        emit speedChanged();
    } else if (propertyName == QStringLiteral("track-list") && format == MPV_FORMAT_NODE) {
        updateTracks();
    } else if (propertyName == QStringLiteral("paused-for-cache") && format == MPV_FORMAT_FLAG) {
        m_buffering = *static_cast<int*>(data) != 0;
        if (!m_buffering) {
            m_bufferingProgress = 0;
        }
        emit playbackStateChanged();
    } else if (propertyName == QStringLiteral("cache-buffering-state") && format == MPV_FORMAT_INT64) {
        m_bufferingProgress = std::clamp(static_cast<int>(*static_cast<int64_t*>(data)), 0, 100);
        emit playbackStateChanged();
    } else if (propertyName == QStringLiteral("seeking") && format == MPV_FORMAT_FLAG) {
        m_seeking = *static_cast<int*>(data) != 0;
        emit playbackStateChanged();
    } else if (propertyName == QStringLiteral("idle-active") && format == MPV_FORMAT_FLAG) {
        const auto idle = *static_cast<int*>(data) != 0;
        if (idle && (m_loading || m_buffering || m_seeking)) {
            m_loading = false;
            m_buffering = false;
            m_seeking = false;
            m_bufferingProgress = 0;
            updateCacheDuration(-1.0);
            emit playbackStateChanged();
        }
    } else if (propertyName == QStringLiteral("demuxer-cache-duration") && format == MPV_FORMAT_DOUBLE) {
        updateCacheDuration(*static_cast<double*>(data));
    }
}

void PlayerController::updateTracks()
{
    if (!m_mpv) {
        return;
    }

    mpv_node root {};
    const auto status = mpv_get_property(m_mpv, "track-list", MPV_FORMAT_NODE, &root);
    if (status < 0) {
        return;
    }

    std::vector<TrackInfo> subtitles;
    std::vector<TrackInfo> audioTracks;
    VideoTrackDetails videoTrack;

    if (root.format == MPV_FORMAT_NODE_ARRAY && root.u.list) {
        for (int i = 0; i < root.u.list->num; ++i) {
            const auto& entry = root.u.list->values[i];
            if (entry.format != MPV_FORMAT_NODE_MAP || !entry.u.list) {
                continue;
            }

            TrackInfo track;
            int videoWidth = -1;
            int videoHeight = -1;
            double videoFrameRate = 0.0;
            qint64 videoBitrate = 0;
            for (int j = 0; j < entry.u.list->num; ++j) {
                const auto key = QString::fromUtf8(entry.u.list->keys[j]);
                const auto& value = entry.u.list->values[j];
                if (key == QStringLiteral("id")) {
                    track.id = nodeInt(value);
                } else if (key == QStringLiteral("type")) {
                    track.type = nodeString(value);
                } else if (key == QStringLiteral("title")) {
                    track.title = nodeString(value);
                } else if (key == QStringLiteral("lang")) {
                    track.language = nodeString(value);
                } else if (key == QStringLiteral("codec")) {
                    track.codec = nodeString(value);
                } else if (key == QStringLiteral("selected")) {
                    track.selected = nodeBool(value);
                } else if (key == QStringLiteral("demux-w")) {
                    videoWidth = nodeInt(value);
                } else if (key == QStringLiteral("demux-h")) {
                    videoHeight = nodeInt(value);
                } else if (key == QStringLiteral("demux-fps")) {
                    videoFrameRate = nodeDouble(value);
                } else if (key == QStringLiteral("demux-bitrate")) {
                    videoBitrate = nodeInt64(value);
                } else if (key == QStringLiteral("hls-bitrate") && videoBitrate <= 0) {
                    videoBitrate = nodeInt64(value);
                }
            }

            if (track.type == QStringLiteral("sub")) {
                subtitles.push_back(track);
            } else if (track.type == QStringLiteral("audio")) {
                audioTracks.push_back(track);
            } else if (track.type == QStringLiteral("video")) {
                const VideoTrackDetails candidate {
                    true,
                    track.selected,
                    track.codec,
                    videoWidth,
                    videoHeight,
                    videoFrameRate,
                    videoBitrate,
                };
                if (!videoTrack.available || candidate.selected) {
                    videoTrack = candidate;
                }
            }
        }
    }

    mpv_free_node_contents(&root);
    m_subtitleTracks.setTracks(std::move(subtitles));
    m_audioTracks.setTracks(std::move(audioTracks));
    if (videoTrack.available) {
        updateVideoInfo(formatVideoResolution(videoTrack.width, videoTrack.height),
                        formatVideoCodec(videoTrack.codec),
                        formatFrameRate(videoTrack.frameRate),
                        formatBitrate(videoTrack.bitrate));
    } else {
        updateVideoInfo({}, {}, {}, {});
    }
    emit tracksChanged();
}

void PlayerController::updateVideoInfo(QString resolution, QString codec, QString frameRate, QString bitrate)
{
    if (m_videoResolution == resolution &&
        m_videoCodec == codec &&
        m_videoFrameRate == frameRate &&
        m_videoBitrate == bitrate) {
        return;
    }

    m_videoResolution = std::move(resolution);
    m_videoCodec = std::move(codec);
    m_videoFrameRate = std::move(frameRate);
    m_videoBitrate = std::move(bitrate);
    emit videoInfoChanged();
}

void PlayerController::updateCacheDuration(double seconds)
{
    const auto normalized = std::isfinite(seconds) && seconds >= 0.0 ? seconds : -1.0;
    if (std::abs(m_cacheDurationSeconds - normalized) < 0.1) {
        return;
    }
    m_cacheDurationSeconds = normalized;
    emit cacheStatsChanged();
}

void PlayerController::resetPlaybackState()
{
    const auto stateChanged = !m_paused || m_loading || m_buffering || m_seeking || m_position != 0.0 || m_duration != 0.0 || m_bufferingProgress != 0;
    m_paused = true;
    m_loading = false;
    m_buffering = false;
    m_seeking = false;
    m_bufferingProgress = 0;
    m_position = 0.0;
    m_duration = 0.0;
    m_subtitleTracks.setTracks({});
    m_audioTracks.setTracks({});
    updateVideoInfo({}, {}, {}, {});
    updateCacheDuration(-1.0);
    emit tracksChanged();
    if (stateChanged) {
        emit playbackStateChanged();
    }
}

QString PlayerController::nodeString(const mpv_node& node)
{
    if (node.format == MPV_FORMAT_STRING && node.u.string) {
        return QString::fromUtf8(node.u.string);
    }
    return {};
}

int PlayerController::nodeInt(const mpv_node& node, int fallback)
{
    if (node.format == MPV_FORMAT_INT64) {
        return static_cast<int>(node.u.int64);
    }
    return fallback;
}

qint64 PlayerController::nodeInt64(const mpv_node& node, qint64 fallback)
{
    if (node.format == MPV_FORMAT_INT64) {
        return static_cast<qint64>(node.u.int64);
    }
    return fallback;
}

double PlayerController::nodeDouble(const mpv_node& node, double fallback)
{
    if (node.format == MPV_FORMAT_DOUBLE) {
        return node.u.double_;
    }
    if (node.format == MPV_FORMAT_INT64) {
        return static_cast<double>(node.u.int64);
    }
    return fallback;
}

bool PlayerController::nodeBool(const mpv_node& node, bool fallback)
{
    if (node.format == MPV_FORMAT_FLAG) {
        return node.u.flag != 0;
    }
    return fallback;
}

bool PlayerController::command(const char** args)
{
    const auto status = mpv_command(m_mpv, args);
    if (status < 0) {
        emit errorOccurred(QString::fromUtf8(mpv_error_string(status)));
        return false;
    }
    return true;
}
