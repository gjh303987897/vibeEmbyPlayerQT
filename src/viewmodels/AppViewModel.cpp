#include "viewmodels/AppViewModel.h"

#include "services/credentials/CredentialStore.h"
#include "services/iptv/IptvParser.h"
#include "utils/AppLogger.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QLocale>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QStandardPaths>
#include <QtMath>
#include <QStringList>
#include <QStyleHints>
#include <QUrl>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <utility>

namespace {
constexpr qint64 usageNetworkFlushBytes = 1024 * 1024;
constexpr qint64 usageWatchFlushSeconds = 15;
constexpr int usageFlushIntervalMs = 15000;
constexpr qint64 playbackTicksPerSecond = 10'000'000;
constexpr int recentPlaybackProgressMergeMs = 30000;
constexpr int continueRefreshAfterStopMs = 1500;

double playbackPercentageForTicks(const MediaItem& item, qint64 positionTicks)
{
    if (item.runTimeTicks > 0) {
        return std::clamp(static_cast<double>(std::max<qint64>(0, positionTicks)) * 100.0 / static_cast<double>(item.runTimeTicks), 0.0, 100.0);
    }
    return std::clamp(item.playedPercentage, 0.0, 100.0);
}

QString displayNetworkError(const NetworkError& error)
{
    if (!error.message.isEmpty()) {
        return error.message;
    }
    switch (error.kind) {
    case NetworkErrorKind::InvalidUrl:
        return QStringLiteral("Invalid server URL");
    case NetworkErrorKind::Timeout:
        return QStringLiteral("Network request timed out");
    case NetworkErrorKind::Ssl:
        return QStringLiteral("TLS certificate error");
    case NetworkErrorKind::Http:
        return QStringLiteral("Server returned HTTP %1").arg(error.httpStatus);
    case NetworkErrorKind::Parse:
        return QStringLiteral("Unable to parse server response");
    case NetworkErrorKind::CertificateRejected:
        return QStringLiteral("Certificate rejected");
    case NetworkErrorKind::Transport:
        return QStringLiteral("Network request failed");
    }
    return QStringLiteral("Network request failed");
}

QString serverIdFor(const QString& baseUrl, ServiceType type)
{
    const auto source = serviceTypeToString(type).toUtf8() + ':' + baseUrl.trimmed().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex());
}

QString cardIdFor(const QString& baseUrl, ServiceType type, const QString& username)
{
    const auto source = serviceTypeToString(type).toUtf8() + ':' + baseUrl.trimmed().toUtf8() + ':' + username.trimmed().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex());
}

QString iptvServiceIdFor(const QString& sourcePath)
{
    const auto source = QByteArray("IPTV:") + sourcePath.trimmed().toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex());
}

QString iptvPlaylistIdFor(const QString& serviceId)
{
    return QStringLiteral("iptv-playlist-%1").arg(serviceId);
}

QString iptvImportDirectory()
{
    const auto directory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("iptv"));
    QDir().mkpath(directory);
    return directory;
}

QString safeFileName(QString value)
{
    value = value.trimmed();
    if (value.isEmpty()) {
        value = QStringLiteral("playlist");
    }
    value.replace(QRegularExpression(QStringLiteral(R"([\\/:*?"<>|]+)")), QStringLiteral("_"));
    return value.left(80);
}

QString defaultIptvGroup()
{
    return QStringLiteral("Default");
}

QString allIptvGroup()
{
    return QStringLiteral("All");
}

QUrl ensureDirectoryUrl(QUrl url)
{
    auto path = url.path();
    if (!path.endsWith(QLatin1Char('/'))) {
        path.append(QLatin1Char('/'));
        url.setPath(path);
    }
    return url;
}

bool isVideoFileName(const QString& name)
{
    const auto suffix = name.section(QLatin1Char('.'), -1).toLower();
    static const QSet<QString> extensions {
        QStringLiteral("mp4"), QStringLiteral("mkv"), QStringLiteral("avi"), QStringLiteral("mov"),
        QStringLiteral("webm"), QStringLiteral("ts"), QStringLiteral("m2ts"), QStringLiteral("flv"),
        QStringLiteral("wmv"), QStringLiteral("mpg"), QStringLiteral("mpeg"), QStringLiteral("m4v"),
        QStringLiteral("3gp"), QStringLiteral("ogv")
    };
    return extensions.contains(suffix);
}

QString sizeText(qint64 bytes)
{
    if (bytes < 0) {
        return QStringLiteral("Unknown size");
    }
    double value = static_cast<double>(bytes);
    QStringList units { QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB"), QStringLiteral("TB") };
    int unit = 0;
    while (value >= 1024.0 && unit < units.size() - 1) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? QStringLiteral("%1 %2").arg(bytes).arg(units[unit])
                     : QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(units[unit]);
}

QString itemMeta(const MediaItem& item)
{
    QStringList parts;
    if (!item.productionYear.isEmpty()) {
        parts.push_back(item.productionYear);
    }
    if (!item.runTime.isEmpty()) {
        parts.push_back(item.runTime);
    }
    if (!item.communityRating.isEmpty()) {
        parts.push_back(QStringLiteral("Rating %1").arg(item.communityRating));
    }
    if (!item.officialRating.isEmpty()) {
        parts.push_back(item.officialRating);
    }
    if (!item.genres.isEmpty()) {
        parts.push_back(item.genres);
    }
    return parts.join(QStringLiteral(" · "));
}

QString normalizedNumberText(const QString& value)
{
    const auto trimmed = value.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    return trimmed;
}

bool isSeriesItem(const MediaItem& item)
{
    return item.itemType.compare(QStringLiteral("Series"), Qt::CaseInsensitive) == 0;
}

bool isEpisodeItem(const MediaItem& item)
{
    return item.itemType.compare(QStringLiteral("Episode"), Qt::CaseInsensitive) == 0;
}

bool hasSeriesEpisodes(const MediaItem& item)
{
    return isSeriesItem(item) || (isEpisodeItem(item) && !item.seriesId.isEmpty());
}

void applyMissingEpisodeContext(MediaItem& item, const MediaItem& context)
{
    if (!isEpisodeItem(item) && !isEpisodeItem(context)) {
        return;
    }
    if (item.itemType.isEmpty()) {
        item.itemType = context.itemType;
    }
    if (item.seriesId.isEmpty()) {
        item.seriesId = context.seriesId;
    }
    if (item.seriesName.isEmpty()) {
        item.seriesName = context.seriesName;
    }
    if (item.seriesImageTag.isEmpty()) {
        item.seriesImageTag = context.seriesImageTag;
    }
    if (item.seriesImageUrl.isEmpty()) {
        item.seriesImageUrl = context.seriesImageUrl;
    }
    if (item.parentId.isEmpty()) {
        item.parentId = context.parentId;
    }
    if (item.parentIndexNumber.isEmpty()) {
        item.parentIndexNumber = context.parentIndexNumber;
    }
    if (item.seasonName.isEmpty()) {
        item.seasonName = context.seasonName;
    }
}

MediaItem seriesContextFor(const MediaItem& item)
{
    auto context = item;
    if (isSeriesItem(context)) {
        context.seriesId = context.id;
        context.seriesName = context.name;
        context.seriesImageTag = context.imageTag;
        context.seriesImageUrl = context.imageUrl;
    }
    context.parentId.clear();
    context.parentIndexNumber.clear();
    context.seasonName.clear();
    return context;
}

QString normalizedLanguage(const QString& mode)
{
    if (mode == QStringLiteral("zh_CN") || mode == QStringLiteral("en_US") || mode == QStringLiteral("system")) {
        return mode;
    }
    return QStringLiteral("system");
}

QString normalizedTheme(const QString& mode)
{
    if (mode == QStringLiteral("system") || mode == QStringLiteral("dark") || mode == QStringLiteral("light")) {
        return mode;
    }
    return QStringLiteral("dark");
}

QString systemLanguage()
{
    const auto name = QLocale::system().name();
    return name.startsWith(QStringLiteral("zh")) ? QStringLiteral("zh_CN") : QStringLiteral("en_US");
}

QString systemTheme()
{
    if (qApp && qApp->styleHints()->colorScheme() == Qt::ColorScheme::Light) {
        return QStringLiteral("light");
    }
    return QStringLiteral("dark");
}

QString effectiveLanguage(const QString& mode)
{
    return mode == QStringLiteral("system") ? systemLanguage() : mode;
}

bool validPinText(const QString& pin)
{
    static const QRegularExpression pattern(QStringLiteral("^\\d{4,12}$"));
    return pattern.match(pin).hasMatch();
}

using TranslationEntry = std::pair<const char*, const char*>;

const QHash<QString, QString>& englishTexts()
{
    static const QHash<QString, QString> texts {
        { QStringLiteral("app.title"), QStringLiteral("vibePlayerQT") },
        { QStringLiteral("nav.services"), QStringLiteral("Services") },
        { QStringLiteral("nav.settings"), QStringLiteral("Settings") },
        { QStringLiteral("nav.history"), QStringLiteral("History") },
        { QStringLiteral("nav.privacy"), QStringLiteral("Privacy mode") },
        { QStringLiteral("nav.chooseSource"), QStringLiteral("Choose or add a media source") },
        { QStringLiteral("action.add"), QStringLiteral("Add") },
        { QStringLiteral("action.edit"), QStringLiteral("Edit") },
        { QStringLiteral("action.done"), QStringLiteral("Done") },
        { QStringLiteral("action.refresh"), QStringLiteral("Refresh") },
        { QStringLiteral("action.backToServices"), QStringLiteral("Services") },
        { QStringLiteral("action.dismiss"), QStringLiteral("Dismiss") },
        { QStringLiteral("action.save"), QStringLiteral("Save") },
        { QStringLiteral("action.cancel"), QStringLiteral("Cancel") },
        { QStringLiteral("action.delete"), QStringLiteral("Delete") },
        { QStringLiteral("action.upload"), QStringLiteral("Upload") },
        { QStringLiteral("action.uploadFolder"), QStringLiteral("Upload folder") },
        { QStringLiteral("action.download"), QStringLiteral("Download") },
        { QStringLiteral("action.transfers"), QStringLiteral("Transfers") },
        { QStringLiteral("action.choose"), QStringLiteral("Choose") },
        { QStringLiteral("action.play"), QStringLiteral("Play") },
        { QStringLiteral("action.continue"), QStringLiteral("Continue") },
        { QStringLiteral("action.pause"), QStringLiteral("Pause") },
        { QStringLiteral("action.resume"), QStringLiteral("Resume") },
        { QStringLiteral("action.stop"), QStringLiteral("Stop") },
        { QStringLiteral("action.exitPlayback"), QStringLiteral("Exit") },
        { QStringLiteral("action.fullscreen"), QStringLiteral("Fullscreen") },
        { QStringLiteral("action.exitFullscreen"), QStringLiteral("Exit Fullscreen") },
        { QStringLiteral("action.forward15"), QStringLiteral("+15s") },
        { QStringLiteral("action.rewind15"), QStringLiteral("-15s") },
        { QStringLiteral("dialog.certificateTitle"), QStringLiteral("Certificate confirmation") },
        { QStringLiteral("dialog.certificatePrefix"), QStringLiteral("The server certificate for ") },
        { QStringLiteral("dialog.certificateSuffix"), QStringLiteral(" cannot be verified. Continue for this request?") },
        { QStringLiteral("dialog.passwordTitle"), QStringLiteral("Password required") },
        { QStringLiteral("dialog.serviceTitle"), QStringLiteral("Service") },
        { QStringLiteral("dialog.deleteTitle"), QStringLiteral("Delete service") },
        { QStringLiteral("dialog.deletePrompt"), QStringLiteral("Remove this service card?") },
        { QStringLiteral("dialog.deleteLocalData"), QStringLiteral("Also delete local token and cached data") },
        { QStringLiteral("dialog.exitPlaybackTitle"), QStringLiteral("Exit playback?") },
        { QStringLiteral("dialog.exitPlaybackPrompt"), QStringLiteral("Playback will stop and you will return to the media details page.") },
        { QStringLiteral("dialog.overviewTitle"), QStringLiteral("Overview") },
        { QStringLiteral("form.serviceName"), QStringLiteral("Service name") },
        { QStringLiteral("form.serverUrl"), QStringLiteral("https://server.example.com") },
        { QStringLiteral("form.webDavEndpoint"), QStringLiteral("https://server.example.com/webdav/") },
        { QStringLiteral("form.username"), QStringLiteral("Username") },
        { QStringLiteral("form.password"), QStringLiteral("Password") },
        { QStringLiteral("form.autoLogin"), QStringLiteral("Auto login") },
        { QStringLiteral("form.selfSigned"), QStringLiteral("Allow self-signed certificate prompt") },
        { QStringLiteral("iptv.selectFile"), QStringLiteral("Select IPTV playlist") },
        { QStringLiteral("iptv.filePlaceholder"), QStringLiteral("M3U or M3U8 playlist file") },
        { QStringLiteral("iptv.chooseFile"), QStringLiteral("Choose file") },
        { QStringLiteral("iptv.playlist"), QStringLiteral("Playlist") },
        { QStringLiteral("iptv.localFile"), QStringLiteral("Local file") },
        { QStringLiteral("iptv.title"), QStringLiteral("IPTV Channels") },
        { QStringLiteral("iptv.channels"), QStringLiteral("channels") },
        { QStringLiteral("iptv.playerChannels"), QStringLiteral("Channels") },
        { QStringLiteral("iptv.nowPlaying"), QStringLiteral("On air") },
        { QStringLiteral("iptv.search"), QStringLiteral("Search channels") },
        { QStringLiteral("iptv.allGroups"), QStringLiteral("All") },
        { QStringLiteral("iptv.noChannels"), QStringLiteral("No channels found") },
        { QStringLiteral("webdav.title"), QStringLiteral("WebDAV Files") },
        { QStringLiteral("webdav.empty"), QStringLiteral("This folder is empty") },
        { QStringLiteral("webdav.loadingFolder"), QStringLiteral("Loading folder...") },
        { QStringLiteral("webdav.loadingHint"), QStringLiteral("Reading remote directory") },
        { QStringLiteral("webdav.defaultDownload"), QStringLiteral("Default download folder") },
        { QStringLiteral("webdav.noDownloadFolder"), QStringLiteral("Ask every time") },
        { QStringLiteral("webdav.spaceWarningTitle"), QStringLiteral("Storage check") },
        { QStringLiteral("webdav.spaceWarning"), QStringLiteral("Download size is %1, available disk space is %2. Continue anyway?") },
        { QStringLiteral("webdav.unknownSizeWarning"), QStringLiteral("The total download size could not be confirmed. Continue anyway?") },
        { QStringLiteral("transfers.title"), QStringLiteral("Transfers") },
        { QStringLiteral("transfers.subtitle"), QStringLiteral("Download queue and recent activity") },
        { QStringLiteral("transfers.detailsSubtitle"), QStringLiteral("File progress for this download") },
        { QStringLiteral("transfers.empty"), QStringLiteral("No transfer tasks") },
        { QStringLiteral("transfers.emptyHint"), QStringLiteral("Downloads and uploads will appear here") },
        { QStringLiteral("transfers.emptyDetails"), QStringLiteral("This download contains no file tasks") },
        { QStringLiteral("transfers.files"), QStringLiteral("files") },
        { QStringLiteral("transfers.pending"), QStringLiteral("Pending") },
        { QStringLiteral("transfers.completed"), QStringLiteral("Completed") },
        { QStringLiteral("transfers.failed"), QStringLiteral("Failed / canceled") },
        { QStringLiteral("transfers.speed"), QStringLiteral("Current speed") },
        { QStringLiteral("transfers.averageSpeed"), QStringLiteral("Average speed") },
        { QStringLiteral("transfers.downloadRate"), QStringLiteral("Download") },
        { QStringLiteral("transfers.uploadRate"), QStringLiteral("Upload") },
        { QStringLiteral("transfers.remaining"), QStringLiteral("Remaining download") },
        { QStringLiteral("transfers.unknown"), QStringLiteral("Calculating") },
        { QStringLiteral("transfers.openDetails"), QStringLiteral("View file progress") },
        { QStringLiteral("transfers.clearFinished"), QStringLiteral("Clear finished") },
        { QStringLiteral("transfers.statusQueued"), QStringLiteral("Queued") },
        { QStringLiteral("transfers.statusRunning"), QStringLiteral("Downloading") },
        { QStringLiteral("transfers.statusUploading"), QStringLiteral("Uploading") },
        { QStringLiteral("transfers.statusCreatingFolder"), QStringLiteral("Creating folder") },
        { QStringLiteral("transfers.statusPaused"), QStringLiteral("Paused") },
        { QStringLiteral("transfers.statusDone"), QStringLiteral("Completed") },
        { QStringLiteral("transfers.statusFailed"), QStringLiteral("Failed") },
        { QStringLiteral("transfers.statusCanceled"), QStringLiteral("Canceled") },
        { QStringLiteral("transfers.pause"), QStringLiteral("Pause") },
        { QStringLiteral("transfers.resume"), QStringLiteral("Resume") },
        { QStringLiteral("transfers.retryTask"), QStringLiteral("Retry all failed files") },
        { QStringLiteral("transfers.retryFile"), QStringLiteral("Retry this file") },
        { QStringLiteral("transfers.cancelTask"), QStringLiteral("Cancel task and delete local files") },
        { QStringLiteral("transfers.cancelFile"), QStringLiteral("Cancel file") },
        { QStringLiteral("status.autoLogin"), QStringLiteral("Auto login") },
        { QStringLiteral("status.passwordRequired"), QStringLiteral("Password required") },
        { QStringLiteral("status.ready"), QStringLiteral("Ready") },
        { QStringLiteral("status.noSession"), QStringLiteral("No session") },
        { QStringLiteral("empty.noServices"), QStringLiteral("No services yet") },
        { QStringLiteral("empty.addService"), QStringLiteral("Add service") },
        { QStringLiteral("section.continueWatching"), QStringLiteral("Continue Watching") },
        { QStringLiteral("section.continueSubtitle"), QStringLiteral("Resume progress opens the media details page") },
        { QStringLiteral("section.noProgress"), QStringLiteral("Nothing in progress") },
        { QStringLiteral("section.libraries"), QStringLiteral("Libraries") },
        { QStringLiteral("section.librariesSubtitle"), QStringLiteral("Browse server media categories") },
        { QStringLiteral("loading.home"), QStringLiteral("Loading home") },
        { QStringLiteral("loading.homeHint"), QStringLiteral("Fetching libraries and resume items") },
        { QStringLiteral("loading.library"), QStringLiteral("Loading library") },
        { QStringLiteral("loading.libraryHint"), QStringLiteral("Reading media items from the server") },
        { QStringLiteral("details.noOverview"), QStringLiteral("No overview available.") },
        { QStringLiteral("details.showOverview"), QStringLiteral("Show overview") },
        { QStringLiteral("details.seasonsEpisodes"), QStringLiteral("Seasons & Episodes") },
        { QStringLiteral("details.noSeasons"), QStringLiteral("No seasons available") },
        { QStringLiteral("details.castCrew"), QStringLiteral("Cast & Crew") },
        { QStringLiteral("details.noCast"), QStringLiteral("No cast information available") },
        { QStringLiteral("player.subtitles"), QStringLiteral("Subtitles") },
        { QStringLiteral("player.noSubtitles"), QStringLiteral("No subtitles") },
        { QStringLiteral("player.subtitleOff"), QStringLiteral("Off") },
        { QStringLiteral("player.audio"), QStringLiteral("Audio") },
        { QStringLiteral("player.tracks"), QStringLiteral("tracks") },
        { QStringLiteral("player.noAudioTracks"), QStringLiteral("No audio tracks") },
        { QStringLiteral("player.speed"), QStringLiteral("Speed") },
        { QStringLiteral("player.current"), QStringLiteral("Current") },
        { QStringLiteral("player.currentSpeed"), QStringLiteral("Current speed") },
        { QStringLiteral("player.volume"), QStringLiteral("Volume") },
        { QStringLiteral("player.loading"), QStringLiteral("Loading video") },
        { QStringLiteral("player.buffering"), QStringLiteral("Buffering") },
        { QStringLiteral("player.seeking"), QStringLiteral("Seeking") },
        { QStringLiteral("player.networkHint"), QStringLiteral("Waiting for the stream") },
        { QStringLiteral("player.info"), QStringLiteral("Info") },
        { QStringLiteral("player.videoInfo"), QStringLiteral("Video Info") },
        { QStringLiteral("player.resolution"), QStringLiteral("Resolution") },
        { QStringLiteral("player.codec"), QStringLiteral("Codec") },
        { QStringLiteral("player.frameRate"), QStringLiteral("Frame rate") },
        { QStringLiteral("player.bitrate"), QStringLiteral("Bitrate") },
        { QStringLiteral("player.cacheDuration"), QStringLiteral("Cached") },
        { QStringLiteral("player.cacheShort"), QStringLiteral("Cache") },
        { QStringLiteral("player.infoHint"), QStringLiteral("Detected from mpv playback and cache state") },
        { QStringLiteral("settings.title"), QStringLiteral("Settings") },
        { QStringLiteral("settings.subtitle"), QStringLiteral("Appearance, language, and desktop behavior") },
        { QStringLiteral("settings.appearance"), QStringLiteral("Appearance") },
        { QStringLiteral("settings.theme"), QStringLiteral("Theme") },
        { QStringLiteral("settings.language"), QStringLiteral("Language") },
        { QStringLiteral("settings.desktop"), QStringLiteral("Desktop") },
        { QStringLiteral("settings.webdav"), QStringLiteral("WebDAV") },
        { QStringLiteral("settings.privacy"), QStringLiteral("Privacy") },
        { QStringLiteral("settings.privacyPin"), QStringLiteral("Privacy PIN") },
        { QStringLiteral("settings.minimizeToTray"), QStringLiteral("Minimize to tray") },
        { QStringLiteral("history.title"), QStringLiteral("History Stats") },
        { QStringLiteral("history.subtitle"), QStringLiteral("Viewing time and network usage from the last 30 days") },
        { QStringLiteral("history.totalWatch"), QStringLiteral("Total watch time") },
        { QStringLiteral("history.totalTraffic"), QStringLiteral("Total traffic") },
        { QStringLiteral("history.dailyRecords"), QStringLiteral("Daily records") },
        { QStringLiteral("history.empty"), QStringLiteral("No playback history yet") },
        { QStringLiteral("history.service"), QStringLiteral("Service") },
        { QStringLiteral("history.watch"), QStringLiteral("Watch time") },
        { QStringLiteral("history.traffic"), QStringLiteral("Traffic") },
        { QStringLiteral("history.normalTraffic"), QStringLiteral("Normal traffic") },
        { QStringLiteral("history.keepAliveTraffic"), QStringLiteral("Keep-alive traffic") },
        { QStringLiteral("history.download"), QStringLiteral("In") },
        { QStringLiteral("history.upload"), QStringLiteral("Out") },
        { QStringLiteral("history.retention"), QStringLiteral("Stats are kept for 30 days and old records are removed automatically.") },
        { QStringLiteral("history.privateBadge"), QStringLiteral("Private") },
        { QStringLiteral("history.subtitlePrivacy"), QStringLiteral("Privacy mode includes private records from the last 30 days") },
        { QStringLiteral("privacy.editCards"), QStringLiteral("Privacy Cards") },
        { QStringLiteral("privacy.noCards"), QStringLiteral("No private service cards") },
        { QStringLiteral("privacy.pinTitle"), QStringLiteral("Enter privacy PIN") },
        { QStringLiteral("privacy.pinPlaceholder"), QStringLiteral("PIN") },
        { QStringLiteral("privacy.oldPin"), QStringLiteral("Old PIN") },
        { QStringLiteral("privacy.newPin"), QStringLiteral("New PIN") },
        { QStringLiteral("privacy.confirmPin"), QStringLiteral("Confirm PIN") },
        { QStringLiteral("privacy.pinConfigured"), QStringLiteral("PIN configured") },
        { QStringLiteral("privacy.pinMissing"), QStringLiteral("Set a PIN before entering privacy mode") },
        { QStringLiteral("privacy.setPin"), QStringLiteral("Set PIN") },
        { QStringLiteral("privacy.changePin"), QStringLiteral("Change PIN") },
        { QStringLiteral("privacy.editorTitle"), QStringLiteral("Privacy card editor") },
        { QStringLiteral("privacy.editorHint"), QStringLiteral("Select service cards that should only appear in privacy mode.") },
        { QStringLiteral("privacy.pinMismatch"), QStringLiteral("The new PIN entries do not match") },
        { QStringLiteral("privacy.pinInvalid"), QStringLiteral("PIN must be 4 to 12 digits") },
        { QStringLiteral("privacy.pinWrong"), QStringLiteral("Incorrect PIN") },
        { QStringLiteral("privacy.pinSaved"), QStringLiteral("Privacy PIN updated") },
        { QStringLiteral("privacy.entered"), QStringLiteral("Privacy mode enabled") },
        { QStringLiteral("privacy.exited"), QStringLiteral("Privacy mode disabled") },
        { QStringLiteral("option.system"), QStringLiteral("Follow system") },
        { QStringLiteral("option.dark"), QStringLiteral("Dark") },
        { QStringLiteral("option.light"), QStringLiteral("Light") },
        { QStringLiteral("option.zh"), QStringLiteral("简体中文") },
        { QStringLiteral("option.en"), QStringLiteral("English") },
        { QStringLiteral("nav.scheduledTasks"), QStringLiteral("Keep-Alive Tasks") },
        { QStringLiteral("schedule.subtitle"), QStringLiteral("Manually start silent background playback for Emby sources") },
        { QStringLiteral("schedule.add"), QStringLiteral("New Playback") },
        { QStringLiteral("schedule.edit"), QStringLiteral("Edit Playback") },
        { QStringLiteral("schedule.source"), QStringLiteral("Emby source") },
        { QStringLiteral("schedule.duration"), QStringLiteral("Playback duration") },
        { QStringLiteral("schedule.minutes"), QStringLiteral("minutes") },
        { QStringLiteral("schedule.runNow"), QStringLiteral("Start Now") },
        { QStringLiteral("schedule.startHint"), QStringLiteral("Playback starts immediately after saving. If normal playback is active, it waits in the background.") },
        { QStringLiteral("schedule.empty"), QStringLiteral("No saved background playback configurations") },
        { QStringLiteral("schedule.noSources"), QStringLiteral("Add and sign in to an Emby source first") },
        { QStringLiteral("schedule.statusIdle"), QStringLiteral("Ready to start") },
        { QStringLiteral("schedule.statusWaiting"), QStringLiteral("Waiting for foreground playback to finish") },
        { QStringLiteral("schedule.statusStarting"), QStringLiteral("Preparing background playback") },
        { QStringLiteral("schedule.statusPlaying"), QStringLiteral("Playing in background") },
        { QStringLiteral("schedule.statusCompleted"), QStringLiteral("Playback duration completed") },
        { QStringLiteral("schedule.statusError"), QStringLiteral("Task failed") },
        { QStringLiteral("schedule.progress"), QStringLiteral("Progress") },
        { QStringLiteral("schedule.manual"), QStringLiteral("Manual start") },
        { QStringLiteral("schedule.savedConfigs"), QStringLiteral("saved configurations") },
        { QStringLiteral("schedule.deleteTitle"), QStringLiteral("Delete playback configuration") },
        { QStringLiteral("schedule.deletePrompt"), QStringLiteral("Delete this background playback configuration?") },
        { QStringLiteral("schedule.errorSource"), QStringLiteral("Select an Emby source with a saved session") },
        { QStringLiteral("schedule.errorBusy"), QStringLiteral("Another background playback task is already active") },
    };
    return texts;
}

const QHash<QString, QString>& chineseTexts()
{
    static const QHash<QString, QString> texts {
        { QStringLiteral("dialog.overviewTitle"), QStringLiteral("简介") },
        { QStringLiteral("details.showOverview"), QStringLiteral("显示简介") },
        { QStringLiteral("app.title"), QStringLiteral("vibePlayerQT") },
        { QStringLiteral("nav.services"), QStringLiteral("服务") },
        { QStringLiteral("nav.settings"), QStringLiteral("设置") },
        { QStringLiteral("nav.chooseSource"), QStringLiteral("选择或添加媒体来源") },
        { QStringLiteral("action.add"), QStringLiteral("添加") },
        { QStringLiteral("action.edit"), QStringLiteral("编辑") },
        { QStringLiteral("action.done"), QStringLiteral("完成") },
        { QStringLiteral("action.refresh"), QStringLiteral("刷新") },
        { QStringLiteral("action.backToServices"), QStringLiteral("服务") },
        { QStringLiteral("action.dismiss"), QStringLiteral("关闭") },
        { QStringLiteral("action.save"), QStringLiteral("保存") },
        { QStringLiteral("action.cancel"), QStringLiteral("取消") },
        { QStringLiteral("action.delete"), QStringLiteral("删除") },
        { QStringLiteral("action.play"), QStringLiteral("播放") },
        { QStringLiteral("action.continue"), QStringLiteral("继续播放") },
        { QStringLiteral("action.pause"), QStringLiteral("暂停") },
        { QStringLiteral("action.resume"), QStringLiteral("继续") },
        { QStringLiteral("action.stop"), QStringLiteral("停止") },
        { QStringLiteral("action.exitPlayback"), QStringLiteral("退出") },
        { QStringLiteral("action.fullscreen"), QStringLiteral("全屏") },
        { QStringLiteral("action.exitFullscreen"), QStringLiteral("退出全屏") },
        { QStringLiteral("action.forward15"), QStringLiteral("+15 秒") },
        { QStringLiteral("action.rewind15"), QStringLiteral("-15 秒") },
        { QStringLiteral("dialog.certificateTitle"), QStringLiteral("证书确认") },
        { QStringLiteral("dialog.certificatePrefix"), QStringLiteral("服务器 ") },
        { QStringLiteral("dialog.certificateSuffix"), QStringLiteral(" 的证书无法验证。是否继续本次请求？") },
        { QStringLiteral("dialog.passwordTitle"), QStringLiteral("需要密码") },
        { QStringLiteral("dialog.serviceTitle"), QStringLiteral("服务") },
        { QStringLiteral("dialog.deleteTitle"), QStringLiteral("删除服务") },
        { QStringLiteral("dialog.deletePrompt"), QStringLiteral("移除此服务卡片？") },
        { QStringLiteral("dialog.deleteLocalData"), QStringLiteral("同时删除本地 Token 和缓存数据") },
        { QStringLiteral("dialog.exitPlaybackTitle"), QStringLiteral("退出播放？") },
        { QStringLiteral("dialog.exitPlaybackPrompt"), QStringLiteral("播放将停止，并返回当前媒体详情页。") },
        { QStringLiteral("form.serviceName"), QStringLiteral("服务名称") },
        { QStringLiteral("form.serverUrl"), QStringLiteral("https://server.example.com") },
        { QStringLiteral("form.username"), QStringLiteral("用户名") },
        { QStringLiteral("form.password"), QStringLiteral("密码") },
        { QStringLiteral("form.autoLogin"), QStringLiteral("自动登录") },
        { QStringLiteral("form.selfSigned"), QStringLiteral("允许自签名证书确认") },
        { QStringLiteral("status.autoLogin"), QStringLiteral("自动登录") },
        { QStringLiteral("status.passwordRequired"), QStringLiteral("需要密码") },
        { QStringLiteral("status.ready"), QStringLiteral("可用") },
        { QStringLiteral("status.noSession"), QStringLiteral("无会话") },
        { QStringLiteral("empty.noServices"), QStringLiteral("还没有服务") },
        { QStringLiteral("empty.addService"), QStringLiteral("添加服务") },
        { QStringLiteral("section.continueWatching"), QStringLiteral("继续观看") },
        { QStringLiteral("section.continueSubtitle"), QStringLiteral("点击后进入媒体详情页") },
        { QStringLiteral("section.noProgress"), QStringLiteral("暂无继续观看内容") },
        { QStringLiteral("section.libraries"), QStringLiteral("媒体库") },
        { QStringLiteral("section.librariesSubtitle"), QStringLiteral("浏览服务器媒体分类") },
        { QStringLiteral("loading.home"), QStringLiteral("正在加载主页面") },
        { QStringLiteral("loading.homeHint"), QStringLiteral("正在读取媒体库和继续观看") },
        { QStringLiteral("loading.library"), QStringLiteral("正在加载媒体库") },
        { QStringLiteral("loading.libraryHint"), QStringLiteral("正在从服务器读取媒体条目") },
        { QStringLiteral("details.noOverview"), QStringLiteral("暂无简介。") },
        { QStringLiteral("details.seasonsEpisodes"), QStringLiteral("季与剧集") },
        { QStringLiteral("details.noSeasons"), QStringLiteral("暂无季集信息") },
        { QStringLiteral("details.castCrew"), QStringLiteral("演职人员") },
        { QStringLiteral("details.noCast"), QStringLiteral("暂无演职人员信息") },
        { QStringLiteral("player.subtitles"), QStringLiteral("字幕") },
        { QStringLiteral("player.noSubtitles"), QStringLiteral("无字幕") },
        { QStringLiteral("player.subtitleOff"), QStringLiteral("关闭") },
        { QStringLiteral("player.audio"), QStringLiteral("音轨") },
        { QStringLiteral("player.tracks"), QStringLiteral("条轨道") },
        { QStringLiteral("player.noAudioTracks"), QStringLiteral("无音轨") },
        { QStringLiteral("player.speed"), QStringLiteral("倍速") },
        { QStringLiteral("player.current"), QStringLiteral("当前") },
        { QStringLiteral("player.currentSpeed"), QStringLiteral("当前倍速") },
        { QStringLiteral("player.volume"), QStringLiteral("音量") },
        { QStringLiteral("player.loading"), QStringLiteral("正在加载视频") },
        { QStringLiteral("player.buffering"), QStringLiteral("正在缓冲") },
        { QStringLiteral("player.seeking"), QStringLiteral("正在定位") },
        { QStringLiteral("player.networkHint"), QStringLiteral("正在等待视频流") },
        { QStringLiteral("settings.title"), QStringLiteral("设置") },
        { QStringLiteral("settings.subtitle"), QStringLiteral("外观、语言和桌面行为") },
        { QStringLiteral("settings.appearance"), QStringLiteral("外观") },
        { QStringLiteral("settings.theme"), QStringLiteral("主题") },
        { QStringLiteral("settings.language"), QStringLiteral("语言") },
        { QStringLiteral("settings.desktop"), QStringLiteral("桌面") },
        { QStringLiteral("settings.minimizeToTray"), QStringLiteral("最小化到托盘") },
        { QStringLiteral("option.system"), QStringLiteral("跟随系统") },
        { QStringLiteral("option.dark"), QStringLiteral("暗黑") },
        { QStringLiteral("option.light"), QStringLiteral("白色") },
        { QStringLiteral("option.zh"), QStringLiteral("简体中文") },
        { QStringLiteral("option.en"), QStringLiteral("English") },
        { QStringLiteral("player.info"), QStringLiteral("视频信息") },
        { QStringLiteral("player.videoInfo"), QStringLiteral("视频信息") },
        { QStringLiteral("player.resolution"), QStringLiteral("分辨率") },
        { QStringLiteral("player.codec"), QStringLiteral("编码格式") },
        { QStringLiteral("player.frameRate"), QStringLiteral("帧率") },
        { QStringLiteral("player.bitrate"), QStringLiteral("码率") },
        { QStringLiteral("player.cacheDuration"), QStringLiteral("已缓存") },
        { QStringLiteral("player.cacheShort"), QStringLiteral("缓存") },
        { QStringLiteral("player.infoHint"), QStringLiteral("根据 mpv 播放与缓存状态自动检测") },
    };
    return texts;
}

const QHash<QString, QString>& iptvChineseTexts()
{
    static const QHash<QString, QString> texts {
        { QStringLiteral("iptv.selectFile"), QStringLiteral("选择 IPTV 播放列表") },
        { QStringLiteral("iptv.filePlaceholder"), QStringLiteral("M3U 或 M3U8 播放列表文件") },
        { QStringLiteral("iptv.chooseFile"), QStringLiteral("选择文件") },
        { QStringLiteral("iptv.playlist"), QStringLiteral("播放列表") },
        { QStringLiteral("iptv.localFile"), QStringLiteral("本地文件") },
        { QStringLiteral("iptv.title"), QStringLiteral("IPTV 频道") },
        { QStringLiteral("iptv.channels"), QStringLiteral("个频道") },
        { QStringLiteral("iptv.playerChannels"), QStringLiteral("频道") },
        { QStringLiteral("iptv.nowPlaying"), QStringLiteral("播放中") },
        { QStringLiteral("iptv.search"), QStringLiteral("搜索频道") },
        { QStringLiteral("iptv.allGroups"), QStringLiteral("全部") },
        { QStringLiteral("iptv.noChannels"), QStringLiteral("没有找到频道") },
    };
    return texts;
}

const QHash<QString, QString>& webDavChineseTexts()
{
    static const QHash<QString, QString> texts {
        { QStringLiteral("action.upload"), QStringLiteral("上传") },
        { QStringLiteral("action.uploadFolder"), QStringLiteral("上传文件夹") },
        { QStringLiteral("action.download"), QStringLiteral("下载") },
        { QStringLiteral("action.transfers"), QStringLiteral("下载任务") },
        { QStringLiteral("action.choose"), QStringLiteral("选择") },
        { QStringLiteral("webdav.title"), QStringLiteral("WebDAV 文件") },
        { QStringLiteral("webdav.empty"), QStringLiteral("当前文件夹为空") },
        { QStringLiteral("webdav.loadingFolder"), QStringLiteral("正在加载文件夹...") },
        { QStringLiteral("webdav.loadingHint"), QStringLiteral("正在读取远程目录") },
        { QStringLiteral("webdav.defaultDownload"), QStringLiteral("默认下载文件夹") },
        { QStringLiteral("webdav.noDownloadFolder"), QStringLiteral("每次询问") },
        { QStringLiteral("webdav.spaceWarningTitle"), QStringLiteral("存储空间检查") },
        { QStringLiteral("webdav.spaceWarning"), QStringLiteral("下载大小为 %1，可用磁盘空间为 %2。仍要继续吗？") },
        { QStringLiteral("webdav.unknownSizeWarning"), QStringLiteral("无法确认下载总大小。仍要继续吗？") },
    };
    return texts;
}

const QHash<QString, QString>& transferChineseTexts()
{
    static const QHash<QString, QString> texts {
        { QStringLiteral("transfers.title"), QStringLiteral("下载任务") },
        { QStringLiteral("transfers.subtitle"), QStringLiteral("下载队列与最近传输记录") },
        { QStringLiteral("transfers.detailsSubtitle"), QStringLiteral("查看本次下载中每个文件的进度") },
        { QStringLiteral("transfers.empty"), QStringLiteral("暂无下载任务") },
        { QStringLiteral("transfers.emptyHint"), QStringLiteral("下载和上传任务会显示在这里") },
        { QStringLiteral("transfers.emptyDetails"), QStringLiteral("本次下载没有文件任务") },
        { QStringLiteral("transfers.files"), QStringLiteral("个文件") },
        { QStringLiteral("transfers.pending"), QStringLiteral("待完成") },
        { QStringLiteral("transfers.completed"), QStringLiteral("已完成") },
        { QStringLiteral("transfers.failed"), QStringLiteral("失败 / 已取消") },
        { QStringLiteral("transfers.speed"), QStringLiteral("当前速度") },
        { QStringLiteral("transfers.averageSpeed"), QStringLiteral("平均速度") },
        { QStringLiteral("transfers.downloadRate"), QStringLiteral("下载") },
        { QStringLiteral("transfers.uploadRate"), QStringLiteral("上传") },
        { QStringLiteral("transfers.remaining"), QStringLiteral("剩余下载量") },
        { QStringLiteral("transfers.unknown"), QStringLiteral("计算中") },
        { QStringLiteral("transfers.openDetails"), QStringLiteral("查看文件进度") },
        { QStringLiteral("transfers.clearFinished"), QStringLiteral("清除已结束") },
        { QStringLiteral("transfers.statusQueued"), QStringLiteral("等待中") },
        { QStringLiteral("transfers.statusRunning"), QStringLiteral("传输中") },
        { QStringLiteral("transfers.statusUploading"), QStringLiteral("上传中") },
        { QStringLiteral("transfers.statusCreatingFolder"), QStringLiteral("创建文件夹") },
        { QStringLiteral("transfers.statusPaused"), QStringLiteral("已暂停") },
        { QStringLiteral("transfers.statusDone"), QStringLiteral("已完成") },
        { QStringLiteral("transfers.statusFailed"), QStringLiteral("失败") },
        { QStringLiteral("transfers.statusCanceled"), QStringLiteral("已取消") },
        { QStringLiteral("transfers.pause"), QStringLiteral("暂停") },
        { QStringLiteral("transfers.resume"), QStringLiteral("继续") },
        { QStringLiteral("transfers.retryTask"), QStringLiteral("重试所有失败文件") },
        { QStringLiteral("transfers.retryFile"), QStringLiteral("重试此文件") },
        { QStringLiteral("transfers.cancelTask"), QStringLiteral("取消任务并删除本地文件") },
        { QStringLiteral("transfers.cancelFile"), QStringLiteral("取消此文件") },
    };
    return texts;
}

const QHash<QString, QString>& historyChineseTexts()
{
    static const QHash<QString, QString> texts {
        { QStringLiteral("nav.history"), QStringLiteral("历史统计") },
        { QStringLiteral("history.title"), QStringLiteral("历史统计") },
        { QStringLiteral("history.subtitle"), QStringLiteral("过去 30 天的观看时长与网络流量") },
        { QStringLiteral("history.totalWatch"), QStringLiteral("总观看") },
        { QStringLiteral("history.totalTraffic"), QStringLiteral("总流量") },
        { QStringLiteral("history.dailyRecords"), QStringLiteral("每日记录") },
        { QStringLiteral("history.empty"), QStringLiteral("暂无播放历史") },
        { QStringLiteral("history.service"), QStringLiteral("服务") },
        { QStringLiteral("history.watch"), QStringLiteral("观看时长") },
        { QStringLiteral("history.traffic"), QStringLiteral("流量") },
        { QStringLiteral("history.normalTraffic"), QStringLiteral("正常流量") },
        { QStringLiteral("history.keepAliveTraffic"), QStringLiteral("保号流量") },
        { QStringLiteral("history.download"), QStringLiteral("入站") },
        { QStringLiteral("history.upload"), QStringLiteral("出站") },
        { QStringLiteral("history.retention"), QStringLiteral("统计数据保留 30 天，过期记录会自动删除。") },
    };
    return texts;
}

const QHash<QString, QString>& privacyChineseTexts()
{
    static const QHash<QString, QString> texts {
        { QStringLiteral("nav.privacy"), QStringLiteral("隐私模式") },
        { QStringLiteral("settings.privacy"), QStringLiteral("隐私") },
        { QStringLiteral("settings.privacyPin"), QStringLiteral("隐私 PIN") },
        { QStringLiteral("history.privateBadge"), QStringLiteral("隐私模式") },
        { QStringLiteral("history.subtitlePrivacy"), QStringLiteral("隐私模式下会包含过去 30 天的隐私记录") },
        { QStringLiteral("privacy.editCards"), QStringLiteral("隐私卡片编辑") },
        { QStringLiteral("privacy.noCards"), QStringLiteral("暂无隐私服务卡片") },
        { QStringLiteral("privacy.pinTitle"), QStringLiteral("输入隐私 PIN") },
        { QStringLiteral("privacy.pinPlaceholder"), QStringLiteral("PIN") },
        { QStringLiteral("privacy.oldPin"), QStringLiteral("旧 PIN") },
        { QStringLiteral("privacy.newPin"), QStringLiteral("新 PIN") },
        { QStringLiteral("privacy.confirmPin"), QStringLiteral("确认 PIN") },
        { QStringLiteral("privacy.pinConfigured"), QStringLiteral("已设置 PIN") },
        { QStringLiteral("privacy.pinMissing"), QStringLiteral("请先在设置中设置隐私 PIN") },
        { QStringLiteral("privacy.setPin"), QStringLiteral("设置 PIN") },
        { QStringLiteral("privacy.changePin"), QStringLiteral("更改 PIN") },
        { QStringLiteral("privacy.editorTitle"), QStringLiteral("隐私卡片编辑") },
        { QStringLiteral("privacy.editorHint"), QStringLiteral("选择只在隐私模式中显示的服务卡片。") },
        { QStringLiteral("privacy.pinMismatch"), QStringLiteral("两次输入的新 PIN 不一致") },
        { QStringLiteral("privacy.pinInvalid"), QStringLiteral("PIN 需要为 4 到 12 位数字") },
        { QStringLiteral("privacy.pinWrong"), QStringLiteral("PIN 错误") },
        { QStringLiteral("privacy.pinSaved"), QStringLiteral("隐私 PIN 已更新") },
        { QStringLiteral("privacy.entered"), QStringLiteral("已进入隐私模式") },
        { QStringLiteral("privacy.exited"), QStringLiteral("已退出隐私模式") },
    };
    return texts;
}

const QHash<QString, QString>& scheduledPlaybackChineseTexts()
{
    static const QHash<QString, QString> texts {
        { QStringLiteral("nav.scheduledTasks"), QStringLiteral("保号任务") },
        { QStringLiteral("schedule.subtitle"), QStringLiteral("手动启动 Emby 无声后台保号播放") },
        { QStringLiteral("schedule.add"), QStringLiteral("新建播放") },
        { QStringLiteral("schedule.edit"), QStringLiteral("编辑播放") },
        { QStringLiteral("schedule.source"), QStringLiteral("Emby 播放源") },
        { QStringLiteral("schedule.duration"), QStringLiteral("播放时长") },
        { QStringLiteral("schedule.minutes"), QStringLiteral("分钟") },
        { QStringLiteral("schedule.runNow"), QStringLiteral("立即开始") },
        { QStringLiteral("schedule.startHint"), QStringLiteral("保存后立即开始；如果正在正常播放，将在后台等待播放结束。") },
        { QStringLiteral("schedule.empty"), QStringLiteral("暂无后台播放配置") },
        { QStringLiteral("schedule.noSources"), QStringLiteral("请先添加并登录 Emby 播放源") },
        { QStringLiteral("schedule.statusIdle"), QStringLiteral("可以立即开始") },
        { QStringLiteral("schedule.statusWaiting"), QStringLiteral("等待前台播放结束") },
        { QStringLiteral("schedule.statusStarting"), QStringLiteral("正在准备后台播放") },
        { QStringLiteral("schedule.statusPlaying"), QStringLiteral("正在后台播放") },
        { QStringLiteral("schedule.statusCompleted"), QStringLiteral("本次播放时长已完成") },
        { QStringLiteral("schedule.statusError"), QStringLiteral("任务执行失败") },
        { QStringLiteral("schedule.progress"), QStringLiteral("播放进度") },
        { QStringLiteral("schedule.manual"), QStringLiteral("手动启动") },
        { QStringLiteral("schedule.savedConfigs"), QStringLiteral("个已保存配置") },
        { QStringLiteral("schedule.deleteTitle"), QStringLiteral("删除播放配置") },
        { QStringLiteral("schedule.deletePrompt"), QStringLiteral("确定删除这个后台播放配置吗？") },
        { QStringLiteral("schedule.errorSource"), QStringLiteral("请选择已保存登录会话的 Emby 播放源") },
        { QStringLiteral("schedule.errorBusy"), QStringLiteral("已有后台播放任务正在运行") },
    };
    return texts;
}
}

AppViewModel::AppViewModel(QObject* parent)
    : QObject(parent)
    , m_embyClient(m_embyNetworkClient, this)
    , m_jellyfinClient(m_jellyfinNetworkClient, this)
    , m_webDavDownloadPlanner(m_webDavClient)
    , m_scheduledPlaybackManager(m_embyClient, m_repository, this)
{
    wireCertificatePrompt(m_embyClient);
    wireCertificatePrompt(m_jellyfinClient);
    wireWebDavCertificatePrompt();
    wireUsageSignals();
    m_usageFlushTimer.setInterval(usageFlushIntervalMs);
    m_usageFlushTimer.setSingleShot(false);
    connect(&m_usageFlushTimer, &QTimer::timeout, this, [this]() {
        flushPendingUsageStats(m_currentView == QStringLiteral("history"));
    });
    m_usageFlushTimer.start();
    if (auto* app = QCoreApplication::instance()) {
        connect(app, &QCoreApplication::aboutToQuit, this, [this]() {
            m_scheduledPlaybackManager.stop();
            finishPlaybackUsageTracking();
            flushPendingUsageStats(false);
        });
    }
    connect(&m_transferManager, &TransferManager::tasksChanged, this, &AppViewModel::transferTasksChanged);
    connect(&m_transferManager, &TransferManager::selectionChanged, this, &AppViewModel::transferSelectionChanged);
    connect(&m_transferManager, &TransferManager::taskFinished, this, [this](const QString&, bool, const QString&) {
        if (m_currentWebDavCard && m_currentView == QStringLiteral("webdav")) {
            refreshWebDavDirectory();
        }
    });
    connect(&m_scheduledPlaybackManager,
            &ScheduledPlaybackManager::statusChanged,
            this,
            [this]() {
                emit scheduledPlaybackStatusChanged();

                const auto status = m_scheduledPlaybackManager.status();
                if (status == QStringLiteral("completed") ||
                    status == QStringLiteral("error") ||
                    status == QStringLiteral("idle")) {
                    flushPendingUsageStats(m_currentView == QStringLiteral("history"));
                }
            });
}

QString AppViewModel::serverUrl() const
{
    return m_serverUrl;
}

void AppViewModel::setServerUrl(const QString& value)
{
    if (m_serverUrl == value) {
        return;
    }
    m_serverUrl = value;
    emit serverUrlChanged();
}

QString AppViewModel::serverName() const
{
    return m_serverName;
}

void AppViewModel::setServerName(const QString& value)
{
    if (m_serverName == value) {
        return;
    }
    m_serverName = value;
    emit serverNameChanged();
}

QString AppViewModel::username() const
{
    return m_username;
}

void AppViewModel::setUsername(const QString& value)
{
    if (m_username == value) {
        return;
    }
    m_username = value;
    emit usernameChanged();
}

QString AppViewModel::password() const
{
    return m_password;
}

void AppViewModel::setPassword(const QString& value)
{
    if (m_password == value) {
        return;
    }
    m_password = value;
    emit passwordChanged();
}

QString AppViewModel::serviceType() const
{
    return serviceTypeToString(m_serviceType);
}

void AppViewModel::setServiceType(const QString& value)
{
    const auto type = serviceTypeFromString(value);
    if (m_serviceType == type) {
        return;
    }
    m_serviceType = type;
    emit serviceTypeChanged();
}

bool AppViewModel::trustSelfSignedCertificate() const
{
    return m_trustSelfSignedCertificate;
}

void AppViewModel::setTrustSelfSignedCertificate(bool value)
{
    if (m_trustSelfSignedCertificate == value) {
        return;
    }
    m_trustSelfSignedCertificate = value;
    emit trustSelfSignedCertificateChanged();
}

bool AppViewModel::autoLogin() const
{
    return m_autoLogin;
}

void AppViewModel::setAutoLogin(bool value)
{
    if (m_autoLogin == value) {
        return;
    }
    m_autoLogin = value;
    emit autoLoginChanged();
}

QString AppViewModel::iptvFilePath() const
{
    return m_iptvFilePath;
}

void AppViewModel::setIptvFilePath(const QString& value)
{
    const auto normalized = QUrl(value).isLocalFile() ? QUrl(value).toLocalFile() : value;
    if (m_iptvFilePath == normalized) {
        return;
    }
    m_iptvFilePath = normalized;
    emit iptvFilePathChanged();
}

QString AppViewModel::iptvSearchText() const
{
    return m_iptvSearchText;
}

void AppViewModel::setIptvSearchText(const QString& value)
{
    if (m_iptvSearchText == value) {
        return;
    }
    m_iptvSearchText = value;
    emit iptvSearchTextChanged();
    applyIptvFilters();
}

QString AppViewModel::iptvSelectedGroup() const
{
    return m_iptvSelectedGroup;
}

QStringList AppViewModel::iptvGroups() const
{
    return m_iptvGroups;
}

IptvChannelListModel* AppViewModel::iptvChannels()
{
    return &m_iptvChannels;
}

bool AppViewModel::iptvPlaybackActive() const
{
    return m_currentIptvCard.has_value() && !m_currentIptvChannelId.isEmpty();
}

QString AppViewModel::currentIptvChannelId() const
{
    return m_currentIptvChannelId;
}

WebDavItemListModel* AppViewModel::webDavItems()
{
    return &m_webDavItems;
}

QString AppViewModel::webDavCurrentPath() const
{
    return m_webDavCurrentUrl.path(QUrl::FullyDecoded);
}

QString AppViewModel::defaultDownloadDirectory() const
{
    return m_defaultDownloadDirectory;
}

void AppViewModel::setDefaultDownloadDirectory(const QString& value)
{
    if (m_defaultDownloadDirectory == value) {
        return;
    }
    m_defaultDownloadDirectory = value;
    m_repository.setDefaultDownloadDirectory(value);
    emit defaultDownloadDirectoryChanged();
}

TransferTaskListModel* AppViewModel::transferTasks()
{
    return m_transferManager.tasks();
}

TransferTaskListModel* AppViewModel::transferDetailTasks()
{
    return m_transferManager.detailTasks();
}

QString AppViewModel::selectedTransferGroupId() const
{
    return m_transferManager.selectedGroupId();
}

QString AppViewModel::selectedTransferGroupTitle() const
{
    return m_transferManager.selectedGroupTitle();
}

int AppViewModel::activeTransferCount() const
{
    return m_transferManager.activeCount();
}

int AppViewModel::completedTransferCount() const
{
    return m_transferManager.completedCount();
}

int AppViewModel::failedTransferCount() const
{
    return m_transferManager.failedCount();
}

qint64 AppViewModel::transferBytesPerSecond() const
{
    return m_transferManager.bytesPerSecond();
}

qint64 AppViewModel::transferAverageBytesPerSecond() const
{
    return m_transferManager.averageBytesPerSecond();
}

qint64 AppViewModel::transferDownloadBytesPerSecond() const
{
    return m_transferManager.downloadBytesPerSecond();
}

qint64 AppViewModel::transferUploadBytesPerSecond() const
{
    return m_transferManager.uploadBytesPerSecond();
}

qint64 AppViewModel::transferAverageDownloadBytesPerSecond() const
{
    return m_transferManager.averageDownloadBytesPerSecond();
}

qint64 AppViewModel::transferAverageUploadBytesPerSecond() const
{
    return m_transferManager.averageUploadBytesPerSecond();
}

qint64 AppViewModel::transferRemainingBytes() const
{
    return m_transferManager.remainingBytes();
}

QString AppViewModel::playbackHttpUsername() const
{
    return m_playbackHttpUsername;
}

QString AppViewModel::playbackHttpPassword() const
{
    return m_playbackHttpPassword;
}

bool AppViewModel::playbackAllowInsecureTls() const
{
    return m_playbackAllowInsecureTls;
}

bool AppViewModel::editingServices() const
{
    return m_editingServices;
}

void AppViewModel::setEditingServices(bool value)
{
    if (m_editingServices == value) {
        return;
    }
    m_editingServices = value;
    emit editingServicesChanged();
}

bool AppViewModel::minimizeToTray() const
{
    return m_repository.minimizeToTray();
}

void AppViewModel::setMinimizeToTray(bool value)
{
    if (minimizeToTray() == value) {
        return;
    }
    m_repository.setMinimizeToTray(value);
    emit minimizeToTrayChanged();
}

QString AppViewModel::themeMode() const
{
    return m_themeMode;
}

void AppViewModel::setThemeMode(const QString& value)
{
    const auto normalized = normalizedTheme(value);
    if (m_themeMode == normalized) {
        return;
    }
    const auto previousEffective = effectiveTheme();
    m_themeMode = normalized;
    m_repository.setThemeMode(m_themeMode);
    emit themeModeChanged();
    if (effectiveTheme() != previousEffective) {
        emit effectiveThemeChanged();
    }
}

QString AppViewModel::effectiveTheme() const
{
    return m_themeMode == QStringLiteral("system") ? systemTheme() : m_themeMode;
}

QString AppViewModel::languageMode() const
{
    return m_languageMode;
}

void AppViewModel::setLanguageMode(const QString& value)
{
    const auto normalized = normalizedLanguage(value);
    if (m_languageMode == normalized) {
        return;
    }
    m_languageMode = normalized;
    m_repository.setLanguageMode(m_languageMode);
    ++m_translationRevision;
    emit languageModeChanged();
    emit translationsChanged();
}

int AppViewModel::translationRevision() const
{
    return m_translationRevision;
}

bool AppViewModel::loading() const
{
    return m_loading;
}

bool AppViewModel::homeLoading() const
{
    return m_homeLoadingRequests > 0;
}

bool AppViewModel::libraryItemsLoading() const
{
    return m_libraryItemsLoading;
}

bool AppViewModel::loggedIn() const
{
    return m_session.has_value();
}

QString AppViewModel::currentUser() const
{
    return m_session ? m_session->username : m_currentIptvPlaylist ? QStringLiteral("IPTV") : QString {};
}

QString AppViewModel::currentServerName() const
{
    return m_session ? m_session->server.name
        : m_currentIptvCard ? m_currentIptvCard->server.name
        : m_currentWebDavCard ? m_currentWebDavCard->server.name
        : QString {};
}

QString AppViewModel::currentLibraryName() const
{
    if (!m_currentMediaParentName.isEmpty()) {
        return m_currentMediaParentName;
    }
    return m_currentLibrary ? m_currentLibrary->name : QString {};
}

QString AppViewModel::currentView() const
{
    return m_currentView;
}

QString AppViewModel::errorMessage() const
{
    return m_errorMessage;
}

QString AppViewModel::selectedItemName() const
{
    return m_selectedItem ? m_selectedItem->name : QString {};
}

QString AppViewModel::selectedItemType() const
{
    return m_selectedItem ? m_selectedItem->itemType : QString {};
}

QString AppViewModel::selectedItemOverview() const
{
    return m_selectedItem ? m_selectedItem->overview : QString {};
}

QString AppViewModel::selectedItemImageUrl() const
{
    if (!m_selectedItem) {
        return {};
    }
    return m_selectedItem->imageUrl.isEmpty() ? m_selectedItem->seriesImageUrl : m_selectedItem->imageUrl;
}

QString AppViewModel::selectedItemBackdropUrl() const
{
    return m_selectedItem ? m_selectedItem->backdropImageUrl : QString {};
}

QString AppViewModel::selectedItemMeta() const
{
    return m_selectedItem ? itemMeta(*m_selectedItem) : QString {};
}

QString AppViewModel::selectedItemSeasonEpisode() const
{
    return m_selectedItem ? formatSeasonEpisode(m_selectedItem->parentIndexNumber, m_selectedItem->indexNumber) : QString {};
}

QString AppViewModel::selectedItemPeople() const
{
    return m_selectedItem ? m_selectedItem->people : QString {};
}

PersonListModel* AppViewModel::selectedItemPeopleModel()
{
    return &m_selectedPeople;
}

double AppViewModel::selectedItemPlayedPercentage() const
{
    return m_selectedItem ? m_selectedItem->playedPercentage : 0.0;
}

bool AppViewModel::selectedItemIsSeries() const
{
    return m_selectedItem ? isSeriesItem(*m_selectedItem) : false;
}

bool AppViewModel::selectedItemHasSeriesEpisodes() const
{
    return m_selectedItem ? hasSeriesEpisodes(*m_selectedItem) : false;
}

QString AppViewModel::selectedSeasonId() const
{
    return m_selectedSeason ? m_selectedSeason->id : QString {};
}

QString AppViewModel::selectedSeasonName() const
{
    return m_selectedSeason ? m_selectedSeason->name : QString {};
}

QUrl AppViewModel::currentPlaybackUrl() const
{
    return m_currentPlaybackUrl;
}

double AppViewModel::currentPlaybackStartSeconds() const
{
    return m_currentPlaybackStartSeconds;
}

ServiceCardListModel* AppViewModel::services()
{
    return &m_services;
}

ServiceCardListModel* AppViewModel::privacyCards()
{
    return &m_privacyCards;
}

bool AppViewModel::privacyMode() const
{
    return m_privacyMode;
}

bool AppViewModel::privacyPinConfigured() const
{
    return m_repository.privacyPinConfigured();
}

MediaLibraryListModel* AppViewModel::libraries()
{
    return &m_libraries;
}

MediaItemListModel* AppViewModel::continueItems()
{
    return &m_continueItems;
}

MediaItemListModel* AppViewModel::items()
{
    return &m_items;
}

MediaItemListModel* AppViewModel::seriesSeasons()
{
    return &m_seriesSeasons;
}

MediaItemListModel* AppViewModel::seriesEpisodes()
{
    return &m_seriesEpisodes;
}

DailyUsageStatsListModel* AppViewModel::usageStats()
{
    return &m_usageStats;
}

qint64 AppViewModel::historyTotalWatchSeconds() const
{
    return m_historyTotalWatchSeconds;
}

qint64 AppViewModel::historyTotalNetworkBytes() const
{
    return m_historyTotalNetworkBytes;
}

qint64 AppViewModel::historyNormalNetworkBytes() const
{
    return m_historyNormalNetworkBytes;
}

qint64 AppViewModel::historyKeepAliveNetworkBytes() const
{
    return m_historyKeepAliveNetworkBytes;
}

ScheduledPlaybackTaskListModel* AppViewModel::scheduledPlaybackTasks()
{
    return &m_scheduledPlaybackTasks;
}

ServiceCardListModel* AppViewModel::scheduledEmbySources()
{
    return &m_scheduledEmbySources;
}

QString AppViewModel::scheduledPlaybackStatus() const
{
    return m_scheduledPlaybackManager.status();
}

QString AppViewModel::scheduledPlaybackServerName() const
{
    return m_scheduledPlaybackManager.currentServerName();
}

QString AppViewModel::scheduledPlaybackMediaName() const
{
    return m_scheduledPlaybackManager.currentMediaName();
}

QString AppViewModel::scheduledPlaybackError() const
{
    return m_scheduledPlaybackManager.errorMessage();
}

qint64 AppViewModel::scheduledPlaybackElapsedSeconds() const
{
    return m_scheduledPlaybackManager.elapsedSeconds();
}

qint64 AppViewModel::scheduledPlaybackTargetSeconds() const
{
    return m_scheduledPlaybackManager.targetSeconds();
}

bool AppViewModel::scheduledPlaybackActive() const
{
    return m_scheduledPlaybackManager.active();
}

bool AppViewModel::scheduledPlaybackWaiting() const
{
    return m_scheduledPlaybackManager.waiting();
}

int AppViewModel::scheduledTaskSourceIndex() const
{
    return m_scheduledTaskSourceIndex;
}

void AppViewModel::setScheduledTaskSourceIndex(int value)
{
    if (m_scheduledTaskSourceIndex == value) {
        return;
    }
    m_scheduledTaskSourceIndex = value;
    emit scheduledTaskEditorChanged();
}

int AppViewModel::scheduledTaskDurationMinutes() const
{
    return m_scheduledTaskDurationMinutes;
}

void AppViewModel::setScheduledTaskDurationMinutes(int value)
{
    const auto normalized = std::clamp(value, 5, 720);
    if (m_scheduledTaskDurationMinutes == normalized) {
        return;
    }
    m_scheduledTaskDurationMinutes = normalized;
    emit scheduledTaskEditorChanged();
}

void AppViewModel::initialize()
{
    if (auto initResult = m_repository.initialize(); !initResult) {
        setError(initResult.error());
        return;
    }

    m_themeMode = normalizedTheme(m_repository.themeMode());
    m_languageMode = normalizedLanguage(m_repository.languageMode());
    m_defaultDownloadDirectory = m_repository.defaultDownloadDirectory();
    emit themeModeChanged();
    emit effectiveThemeChanged();
    emit languageModeChanged();
    emit defaultDownloadDirectoryChanged();
    emit privacyPinChanged();
    emit translationsChanged();
    refreshServiceCards();
    refreshPrivacyCards();
    refreshUsageStats();
    refreshScheduledEmbySources();
    refreshScheduledPlaybackTasks();
    setCurrentView(QStringLiteral("services"));
}

void AppViewModel::beginAddServiceCard()
{
    m_pendingServiceCard.reset();
    setServerName(QString {});
    setServerUrl(QString {});
    setUsername(QString {});
    setPassword(QString {});
    setIptvFilePath(QString {});
    setServiceType(QStringLiteral("Emby"));
    setAutoLogin(true);
    setTrustSelfSignedCertificate(true);
}

void AppViewModel::login()
{
    clearError();
    const auto server = makeServerConfig();

    if (server.baseUrl.isEmpty() || m_username.trimmed().isEmpty()) {
        setError(QStringLiteral("Server URL and username are required"));
        return;
    }

    startLogin(server, m_password);
}

void AppViewModel::saveServiceCard()
{
    clearError();
    const auto server = makeServerConfig();
    if (server.serviceType == ServiceType::IPTV) {
        if (m_iptvFilePath.trimmed().isEmpty()) {
            setError(QStringLiteral("Select an M3U or M3U8 playlist file"));
            return;
        }

        auto channelsResult = IptvParser::parseFile(m_iptvFilePath);
        if (!channelsResult) {
            setError(channelsResult.error());
            return;
        }

        auto channels = std::move(*channelsResult);
        const auto playlistResult = importIptvPlaylistFile(server, channels);
        if (!playlistResult) {
            setError(playlistResult.error());
            return;
        }

        if (m_pendingServiceCard && m_pendingServiceCard->server.id != server.id) {
            m_repository.deleteServer(m_pendingServiceCard->server.id, false);
        }
        if (auto saveResult = m_repository.saveIptvPlaylist(server, *playlistResult, channels); !saveResult) {
            setError(saveResult.error());
            return;
        }

        m_pendingServiceCard.reset();
        setIptvFilePath(QString {});
        refreshServiceCards();
        setCurrentView(QStringLiteral("services"));
        return;
    }

    if (server.serviceType == ServiceType::WebDAV) {
        if (server.baseUrl.isEmpty() || server.username.isEmpty()) {
            setError(QStringLiteral("WebDAV endpoint and username are required"));
            return;
        }

        if (m_pendingServiceCard && m_pendingServiceCard->server.id != server.id) {
            CredentialStore::deletePassword(m_pendingServiceCard->server.id);
            m_repository.deleteServer(m_pendingServiceCard->server.id, false);
        }

        if (!m_password.isEmpty()) {
            saveWebDavCredentials(server, m_password);
        }

        if (auto saveResult = m_repository.saveServer(server); !saveResult) {
            setError(saveResult.error());
            return;
        }
        m_pendingServiceCard.reset();
        setPassword(QString {});
        refreshServiceCards();
        setCurrentView(QStringLiteral("services"));
        return;
    }

    if (server.baseUrl.isEmpty() || server.username.isEmpty()) {
        setError(QStringLiteral("Server URL and username are required"));
        return;
    }

    if (!m_password.isEmpty()) {
        if (m_pendingServiceCard && m_pendingServiceCard->server.id != server.id) {
            m_repository.deleteServer(m_pendingServiceCard->server.id, false);
        }
        startLogin(server, m_password);
        return;
    }

    if (m_pendingServiceCard && m_pendingServiceCard->server.id != server.id) {
        m_repository.deleteServer(m_pendingServiceCard->server.id, false);
    }
    if (auto saveResult = m_repository.saveServer(server); !saveResult) {
        setError(saveResult.error());
        return;
    }
    m_pendingServiceCard.reset();
    refreshServiceCards();
    setCurrentView(QStringLiteral("services"));
}

void AppViewModel::selectServiceCard(int row)
{
    clearError();
    const auto card = m_services.cardAt(row);
    if (!card) {
        return;
    }

    m_pendingServiceCard = *card;
    setServerUrl(card->server.baseUrl);
    setServerName(card->server.name);
    setUsername(card->server.username);
    setServiceType(serviceTypeToString(card->server.serviceType));
    setTrustSelfSignedCertificate(card->server.trustSelfSignedCertificate);
    setAutoLogin(card->server.autoLogin);
    setIptvFilePath(card->server.serviceType == ServiceType::IPTV ? card->server.baseUrl : QString {});

    if (card->server.serviceType == ServiceType::IPTV) {
        loadIptvService(*card);
        return;
    }
    if (card->server.serviceType == ServiceType::WebDAV) {
        const auto password = loadWebDavPassword(card->server);
        if (card->server.autoLogin && password) {
            loadWebDavService(*card, *password);
            return;
        }
        emit passwordRequired(card->server.name, card->server.username);
        return;
    }

    if (!card->server.autoLogin) {
        emit passwordRequired(card->server.name, card->server.username);
        return;
    }

    const auto sessionResult = m_repository.loadSession(card->server.id);
    if (!sessionResult) {
        setError(sessionResult.error());
        return;
    }

    if (!sessionResult->has_value()) {
        emit passwordRequired(card->server.name, card->server.username);
        return;
    }

    setSession(**sessionResult);
    loadServiceHome();
}

void AppViewModel::editServiceCard(int row)
{
    const auto card = m_services.cardAt(row);
    if (!card) {
        return;
    }

    m_pendingServiceCard = *card;
    setServerUrl(card->server.baseUrl);
    setServerName(card->server.name);
    setUsername(card->server.username);
    setServiceType(serviceTypeToString(card->server.serviceType));
    setTrustSelfSignedCertificate(card->server.trustSelfSignedCertificate);
    setAutoLogin(card->server.autoLogin);
    setIptvFilePath(card->server.serviceType == ServiceType::IPTV ? card->server.baseUrl : QString {});
}

void AppViewModel::loginSelectedService(const QString& password)
{
    clearError();
    if (!m_pendingServiceCard) {
        setError(QStringLiteral("No service is selected"));
        return;
    }
    if (m_pendingServiceCard->server.serviceType == ServiceType::IPTV) {
        loadIptvService(*m_pendingServiceCard);
        return;
    }
    if (m_pendingServiceCard->server.serviceType == ServiceType::WebDAV) {
        if (m_pendingServiceCard->server.autoLogin && !password.isEmpty()) {
            saveWebDavCredentials(m_pendingServiceCard->server, password);
        }
        loadWebDavService(*m_pendingServiceCard, password);
        return;
    }

    startLogin(m_pendingServiceCard->server, password);
}

void AppViewModel::chooseIptvPlaylistFile()
{
    const auto selected = QFileDialog::getOpenFileName(nullptr,
                                                       trText(QStringLiteral("iptv.selectFile")),
                                                       m_iptvFilePath,
                                                       QStringLiteral("IPTV playlists (*.m3u *.m3u8);;All files (*)"));
    if (!selected.isEmpty()) {
        setIptvFilePath(selected);
        if (m_serverName.trimmed().isEmpty()) {
            const QFileInfo fileInfo(selected);
            setServerName(fileInfo.completeBaseName());
        }
    }
}

void AppViewModel::selectIptvGroup(const QString& groupName)
{
    const auto normalized = groupName.isEmpty() ? allIptvGroup() : groupName;
    if (m_iptvSelectedGroup == normalized) {
        return;
    }
    m_iptvSelectedGroup = normalized;
    emit iptvSelectedGroupChanged();
    applyIptvFilters();
}

void AppViewModel::playIptvChannel(int row)
{
    clearError();
    const auto channel = m_iptvChannels.channelAt(row);
    if (!channel) {
        return;
    }

    const auto playbackUrl = QUrl(channel->streamUrl);
    if (!playbackUrl.isValid() && !QFileInfo::exists(channel->streamUrl)) {
        setError(QStringLiteral("IPTV channel URL is invalid"));
        return;
    }

    setForegroundPlaybackActive(true);
    m_currentPlaybackUrl = playbackUrl.scheme().isEmpty() ? QUrl::fromLocalFile(channel->streamUrl) : playbackUrl;
    m_currentIptvChannelId = channel->id;
    m_currentMediaSourceId.clear();
    m_currentPlaySessionId.clear();
    m_currentPlaybackStartSeconds = 0.0;
    m_lastPlaybackReportSeconds = -1.0;
    m_playbackStartedReported = false;

    MediaItem item;
    item.id = channel->id;
    item.name = channel->name;
    item.itemType = QStringLiteral("IPTV");
    item.imageUrl = channel->logoUrl;
    item.genres = channel->groupName;
    m_selectedItem = std::move(item);
    clearSeriesDetails();
    syncSelectedPeople();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("player"));
    AppLogger::info(QStringLiteral("iptv"), QStringLiteral("Opening IPTV channel playback"));
}

void AppViewModel::openWebDavItem(int row)
{
    clearError();
    const auto item = m_webDavItems.itemAt(row);
    if (!item) {
        return;
    }
    if (item->directory) {
        m_webDavHistory.push_back(m_webDavCurrentUrl);
        loadWebDavDirectory(ensureDirectoryUrl(item->url));
        return;
    }
    if (!item->playable) {
        return;
    }

    setForegroundPlaybackActive(true);
    if (!m_webDavPlaybackStreamId.isEmpty()) {
        m_webDavPlaybackProxy.revoke(m_webDavPlaybackStreamId);
        m_webDavPlaybackStreamId.clear();
    }
    const auto proxyUrl = m_webDavPlaybackProxy.streamUrlFor(m_currentWebDavCard->server, m_webDavPassword, item->url);
    m_currentPlaybackUrl = proxyUrl;
    m_currentIptvChannelId.clear();
    m_webDavPlaybackStreamId = proxyUrl.path().section(QLatin1Char('/'), 1, 1);
    m_playbackHttpUsername.clear();
    m_playbackHttpPassword.clear();
    m_playbackAllowInsecureTls = false;
    m_currentMediaSourceId.clear();
    m_currentPlaySessionId.clear();
    m_currentPlaybackStartSeconds = 0.0;
    m_lastPlaybackReportSeconds = -1.0;
    m_playbackStartedReported = false;

    MediaItem media;
    media.id = item->url.toString();
    media.name = item->name;
    media.itemType = QStringLiteral("WebDAV");
    media.overview = item->contentType;
    m_selectedItem = std::move(media);
    clearSeriesDetails();
    syncSelectedPeople();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("player"));
}

void AppViewModel::webDavBack()
{
    if (m_webDavHistory.empty()) {
        backToServices();
        return;
    }
    const auto url = m_webDavHistory.back();
    m_webDavHistory.pop_back();
    loadWebDavDirectory(url);
}

void AppViewModel::refreshWebDavDirectory()
{
    if (!m_webDavCurrentUrl.isEmpty()) {
        loadWebDavDirectory(m_webDavCurrentUrl);
    }
}

void AppViewModel::chooseWebDavUploadFiles()
{
    if (!m_currentWebDavCard) {
        return;
    }
    const auto files = QFileDialog::getOpenFileNames(nullptr, trText(QStringLiteral("action.upload")));
    for (const auto& file : files) {
        const QFileInfo info(file);
        if (!info.exists() || !info.isFile()) {
            continue;
        }
        enqueueWebDavUploadFile(info.absoluteFilePath(), childWebDavUrl(info.fileName(), false));
    }
    openTransfers();
}

void AppViewModel::chooseWebDavUploadFolder()
{
    if (!m_currentWebDavCard) {
        return;
    }
    const auto folder = QFileDialog::getExistingDirectory(nullptr, trText(QStringLiteral("action.uploadFolder")));
    if (folder.isEmpty()) {
        return;
    }
    const QFileInfo rootInfo(folder);
    const auto rootRemote = childWebDavUrl(rootInfo.fileName(), true);
    m_transferManager.enqueueCreateDirectory(m_currentWebDavCard->server, m_webDavPassword, rootRemote);

    QDirIterator iterator(folder, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info(iterator.fileInfo());
        const auto relative = QDir(folder).relativeFilePath(info.absoluteFilePath()).replace(QLatin1Char('\\'), QLatin1Char('/'));
        auto remoteUrl = rootRemote.resolved(QUrl(QString::fromUtf8(QUrl::toPercentEncoding(relative))));
        if (info.isDir()) {
            remoteUrl = ensureDirectoryUrl(remoteUrl);
            m_transferManager.enqueueCreateDirectory(m_currentWebDavCard->server, m_webDavPassword, remoteUrl);
        } else if (info.isFile()) {
            enqueueWebDavUploadFile(info.absoluteFilePath(), remoteUrl);
        }
    }
    openTransfers();
}

void AppViewModel::downloadWebDavItem(int row)
{
    clearError();
    const auto item = m_webDavItems.itemAt(row);
    if (!item || !m_currentWebDavCard) {
        return;
    }

    QString directory = m_defaultDownloadDirectory;
    if (directory.isEmpty()) {
        directory = QFileDialog::getExistingDirectory(nullptr, trText(QStringLiteral("webdav.defaultDownload")));
    }
    if (directory.isEmpty()) {
        return;
    }

    const auto targetPath = uniqueLocalPath(directory, item->name);
    const auto available = QStorageInfo(directory).bytesAvailable();
    const auto server = m_currentWebDavCard->server;
    const auto password = m_webDavPassword;
    const auto downloadTitle = item->name;
    const auto directoryDownload = item->directory;
    if (directoryDownload) {
        setLoading(true);
    }

    m_webDavDownloadPlanner.buildPlan(server,
                                      password,
                                      *item,
                                      targetPath,
                                      [this, server, password, available, directoryDownload, downloadTitle, targetPath](WebDavDownloadPlanResult result) mutable {
        if (directoryDownload) {
            setLoading(false);
        }
        if (!result) {
            setError(displayNetworkError(result.error()));
            return;
        }

        auto launchDownload = [this, server, password, downloadTitle, targetPath](WebDavDownloadPlan plan) mutable {
            for (const auto& localDirectory : plan.directories) {
                if (!QDir().mkpath(localDirectory)) {
                    setError(QStringLiteral("Unable to create local download directory"));
                    return;
                }
            }

            std::vector<TransferManager::DownloadRequest> requests;
            requests.reserve(plan.files.size());
            for (auto& file : plan.files) {
                requests.push_back(TransferManager::DownloadRequest {
                    .remoteUrl = std::move(file.remoteUrl),
                    .localPath = std::move(file.localPath),
                    .totalBytes = file.bytesTotal,
                });
            }
            m_transferManager.enqueueDownloads(server,
                                               password,
                                               downloadTitle,
                                               targetPath,
                                               std::move(requests));
            openTransfers();
        };

        auto plan = std::move(*result);
        const auto shouldWarn = !plan.sizeComplete ||
            (available > 0 && plan.bytesTotal > available);
        if (!shouldWarn) {
            launchDownload(std::move(plan));
            return;
        }

        const auto title = trText(QStringLiteral("webdav.spaceWarningTitle"));
        const auto message = !plan.sizeComplete
            ? trText(QStringLiteral("webdav.unknownSizeWarning"))
            : trText(QStringLiteral("webdav.spaceWarning")).arg(sizeText(plan.bytesTotal), sizeText(available));
        m_pendingDownloadWarningReply = [launchDownload, plan = std::move(plan)](bool accepted) mutable {
            if (accepted) {
                launchDownload(std::move(plan));
            }
        };
        emit downloadSpaceWarningRequested(title, message);
    });
}

void AppViewModel::chooseDefaultDownloadDirectory()
{
    const auto directory = QFileDialog::getExistingDirectory(nullptr,
                                                            trText(QStringLiteral("webdav.defaultDownload")),
                                                            m_defaultDownloadDirectory);
    if (!directory.isEmpty()) {
        setDefaultDownloadDirectory(directory);
    }
}

void AppViewModel::openTransfers()
{
    m_transferManager.clearGroupSelection();
    setCurrentView(QStringLiteral("transfers"));
}

void AppViewModel::cancelTransfer(const QString& taskId)
{
    m_transferManager.cancelTask(taskId);
}

void AppViewModel::pauseTransfer(const QString& taskId)
{
    m_transferManager.pauseTask(taskId);
}

void AppViewModel::resumeTransfer(const QString& taskId)
{
    m_transferManager.resumeTask(taskId);
}

void AppViewModel::retryTransfer(const QString& taskId)
{
    m_transferManager.retryTask(taskId);
}

void AppViewModel::clearFinishedTransfers()
{
    m_transferManager.clearFinished();
}

void AppViewModel::openTransferGroup(const QString& groupId)
{
    m_transferManager.selectGroup(groupId);
}

void AppViewModel::closeTransferGroup()
{
    m_transferManager.clearGroupSelection();
}

bool AppViewModel::unlockPrivacyMode(const QString& pin)
{
    clearError();
    if (m_privacyMode) {
        return true;
    }
    if (!m_repository.privacyPinConfigured()) {
        setError(trText(QStringLiteral("privacy.pinMissing")));
        return false;
    }
    if (!verifyPrivacyPin(pin)) {
        setError(trText(QStringLiteral("privacy.pinWrong")));
        return false;
    }

    m_scheduledPlaybackManager.stop();
    m_privacyMode = true;
    emit privacyModeChanged();
    setEditingServices(false);
    refreshServiceCards();
    refreshPrivacyCards();
    refreshUsageStats();
    refreshScheduledPlaybackTasks();
    setCurrentView(QStringLiteral("services"));
    AppLogger::info(QStringLiteral("privacy"), QStringLiteral("Privacy mode enabled"));
    return true;
}

void AppViewModel::exitPrivacyMode()
{
    if (!m_privacyMode) {
        return;
    }

    m_scheduledPlaybackManager.stop();
    m_privacyMode = false;
    emit privacyModeChanged();
    setEditingServices(false);
    backToServices();
    refreshPrivacyCards();
    refreshUsageStats();
    refreshScheduledPlaybackTasks();
    AppLogger::info(QStringLiteral("privacy"), QStringLiteral("Privacy mode disabled"));
}

void AppViewModel::refreshPrivacyCards()
{
    const auto cardsResult = m_repository.loadAllServiceCards();
    if (!cardsResult) {
        setError(cardsResult.error());
        return;
    }
    m_privacyCards.setCards(*cardsResult);
}

void AppViewModel::setPrivacyCardPrivate(int row, bool privateMode)
{
    clearError();
    const auto card = m_privacyCards.cardAt(row);
    if (!card) {
        return;
    }
    if (auto result = m_repository.setServerPrivateMode(card->server.id, privateMode); !result) {
        setError(result.error());
        return;
    }
    m_scheduledPlaybackManager.stop();
    refreshPrivacyCards();
    refreshServiceCards();
    refreshUsageStats();
    refreshScheduledPlaybackTasks();
}

bool AppViewModel::changePrivacyPin(const QString& oldPin, const QString& newPin, const QString& confirmPin)
{
    clearError();
    const auto configured = m_repository.privacyPinConfigured();
    if (configured && !verifyPrivacyPin(oldPin)) {
        setError(trText(QStringLiteral("privacy.pinWrong")));
        return false;
    }
    if (newPin != confirmPin) {
        setError(trText(QStringLiteral("privacy.pinMismatch")));
        return false;
    }
    if (!pinLooksValid(newPin)) {
        setError(trText(QStringLiteral("privacy.pinInvalid")));
        return false;
    }

    const auto salt = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_repository.setPrivacyPinHash(salt, privacyPinHash(newPin, salt));
    emit privacyPinChanged();
    AppLogger::info(QStringLiteral("privacy"), QStringLiteral("Privacy PIN updated"));
    return true;
}

void AppViewModel::acceptPendingDownloadWarning(bool accepted)
{
    if (m_pendingDownloadWarningReply) {
        auto reply = std::move(m_pendingDownloadWarningReply);
        m_pendingDownloadWarningReply = {};
        reply(accepted);
    }
}

void AppViewModel::startPendingFolderDownload()
{
    if (m_pendingFolderDownload) {
        auto action = std::move(m_pendingFolderDownload);
        m_pendingFolderDownload = {};
        action();
    }
}

void AppViewModel::moveServiceCard(int row, int direction)
{
    clearError();
    const auto card = m_services.cardAt(row);
    if (!card) {
        return;
    }

    if (auto result = m_repository.moveServer(card->server.id, direction, m_privacyMode); !result) {
        setError(result.error());
        return;
    }
    refreshServiceCards();
}

void AppViewModel::moveServiceCardTo(int fromRow, int toRow)
{
    clearError();
    const auto card = m_services.cardAt(fromRow);
    if (!card) {
        return;
    }

    if (auto result = m_repository.moveServerTo(card->server.id, toRow, m_privacyMode); !result) {
        setError(result.error());
        return;
    }
    refreshServiceCards();
}

QString AppViewModel::trText(const QString& key) const
{
    const auto language = effectiveLanguage(m_languageMode);
    if (language == QStringLiteral("zh_CN") &&
        (key.startsWith(QStringLiteral("webdav.")) ||
         key == QStringLiteral("action.upload") ||
         key == QStringLiteral("action.uploadFolder") ||
         key == QStringLiteral("action.download") ||
         key == QStringLiteral("action.transfers") ||
         key == QStringLiteral("action.choose"))) {
        const auto& webDavTable = webDavChineseTexts();
        if (webDavTable.contains(key)) {
            return webDavTable.value(key);
        }
    }
    if (language == QStringLiteral("zh_CN") && key.startsWith(QStringLiteral("transfers."))) {
        const auto& transferTable = transferChineseTexts();
        if (transferTable.contains(key)) {
            return transferTable.value(key);
        }
    }
    if (language == QStringLiteral("zh_CN") &&
        (key == QStringLiteral("nav.scheduledTasks") || key.startsWith(QStringLiteral("schedule.")))) {
        const auto& scheduledPlaybackTable = scheduledPlaybackChineseTexts();
        if (scheduledPlaybackTable.contains(key)) {
            return scheduledPlaybackTable.value(key);
        }
    }
    if (language == QStringLiteral("zh_CN") && key.startsWith(QStringLiteral("iptv."))) {
        const auto& iptvTable = iptvChineseTexts();
        if (iptvTable.contains(key)) {
            return iptvTable.value(key);
        }
    }
    if (language == QStringLiteral("zh_CN") && (key == QStringLiteral("nav.history") || key.startsWith(QStringLiteral("history.")))) {
        const auto& historyTable = historyChineseTexts();
        if (historyTable.contains(key)) {
            return historyTable.value(key);
        }
    }
    if (language == QStringLiteral("zh_CN") && (key == QStringLiteral("nav.privacy") || key == QStringLiteral("settings.privacy") || key == QStringLiteral("settings.privacyPin") || key.startsWith(QStringLiteral("privacy.")))) {
        const auto& privacyTable = privacyChineseTexts();
        if (privacyTable.contains(key)) {
            return privacyTable.value(key);
        }
    }
    const auto& table = language == QStringLiteral("zh_CN") ? chineseTexts() : englishTexts();
    if (table.contains(key)) {
        return table.value(key);
    }
    return englishTexts().value(key, key);
}

QString AppViewModel::formatSeasonEpisode(const QString& season, const QString& episode) const
{
    const auto seasonText = normalizedNumberText(season);
    const auto episodeText = normalizedNumberText(episode);
    QStringList parts;

    const auto language = effectiveLanguage(m_languageMode);
    if (language == QStringLiteral("zh_CN")) {
        if (!seasonText.isEmpty()) {
            parts.push_back(QStringLiteral("第 %1 季").arg(seasonText));
        }
        if (!episodeText.isEmpty()) {
            parts.push_back(QStringLiteral("第 %1 集").arg(episodeText));
        }
        return parts.join(QLatin1Char(' '));
    }

    if (!seasonText.isEmpty()) {
        parts.push_back(QStringLiteral("Season %1").arg(seasonText));
    }
    if (!episodeText.isEmpty()) {
        parts.push_back(QStringLiteral("Episode %1").arg(episodeText));
    }
    return parts.join(QLatin1Char(' '));
}

QString AppViewModel::formatContinueProgress(double percentage) const
{
    const auto value = qBound(0, qRound(percentage), 100);
    const auto language = effectiveLanguage(m_languageMode);
    if (language == QStringLiteral("zh_CN")) {
        return QStringLiteral("观看到 %1%").arg(value);
    }
    return QStringLiteral("Watched %1%").arg(value);
}

void AppViewModel::deleteServiceCard(int row, bool deleteLocalData)
{
    clearError();
    const auto card = m_services.cardAt(row);
    if (!card) {
        return;
    }

    if (auto result = m_repository.deleteServer(card->server.id, deleteLocalData); !result) {
        setError(result.error());
        return;
    }
    if (m_session && m_session->server.id == card->server.id) {
        m_session.reset();
        emit loggedInChanged();
        emit currentUserChanged();
        emit currentServerChanged();
    }
    if (m_currentIptvCard && m_currentIptvCard->server.id == card->server.id) {
        clearIptvState();
        if (m_currentView == QStringLiteral("iptv")) {
            setCurrentView(QStringLiteral("services"));
        }
    }
    if (card->server.serviceType == ServiceType::WebDAV && deleteLocalData) {
        CredentialStore::deletePassword(card->server.id);
    }
    if (m_currentWebDavCard && m_currentWebDavCard->server.id == card->server.id) {
        clearWebDavState();
        if (m_currentView == QStringLiteral("webdav")) {
            setCurrentView(QStringLiteral("services"));
        }
    }
    refreshServiceCards();
}

void AppViewModel::logout()
{
    AppLogger::info(QStringLiteral("auth"), QStringLiteral("Logout requested"));
    m_session.reset();
    m_pendingServiceCard.reset();
    clearIptvState();
    clearWebDavState();
    m_currentLibrary.reset();
    clearMediaDirectoryState();
    m_selectedItem.reset();
    clearSeriesDetails();
    syncSelectedPeople();
    clearCurrentPlayback();
    m_libraries.clear();
    m_continueItems.clear();
    m_items.clear();
    emit loggedInChanged();
    emit currentUserChanged();
    emit currentServerChanged();
    emit currentLibraryChanged();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("services"));
}

void AppViewModel::backToServices()
{
    m_session.reset();
    m_pendingServiceCard.reset();
    clearIptvState();
    clearWebDavState();
    m_currentLibrary.reset();
    clearMediaDirectoryState();
    m_selectedItem.reset();
    clearSeriesDetails();
    syncSelectedPeople();
    clearCurrentPlayback();
    m_libraries.clear();
    m_continueItems.clear();
    m_items.clear();
    emit loggedInChanged();
    emit currentUserChanged();
    emit currentServerChanged();
    emit currentLibraryChanged();
    emit selectedItemChanged();
    emit playbackChanged();
    refreshServiceCards();
    setCurrentView(QStringLiteral("services"));
}

void AppViewModel::backToHome()
{
    if (m_currentWebDavCard) {
        if (m_currentView == QStringLiteral("webdav")) {
            backToServices();
            return;
        }
        clearCurrentPlayback();
        emit playbackChanged();
        setCurrentView(QStringLiteral("webdav"));
        return;
    }
    if (m_currentIptvCard) {
        if (m_currentView == QStringLiteral("iptv")) {
            backToServices();
            return;
        }
        clearCurrentPlayback();
        emit playbackChanged();
        setCurrentView(QStringLiteral("iptv"));
        return;
    }

    m_currentLibrary.reset();
    clearMediaDirectoryState();
    m_selectedItem.reset();
    clearSeriesDetails();
    syncSelectedPeople();
    clearCurrentPlayback();
    m_items.clear();
    emit currentLibraryChanged();
    emit selectedItemChanged();
    emit playbackChanged();
    if (m_session) {
        setCurrentView(QStringLiteral("home"));
    } else {
        setCurrentView(QStringLiteral("services"));
    }
}

void AppViewModel::mediaLibraryBack()
{
    if (!m_currentLibrary) {
        backToHome();
        return;
    }

    if (m_mediaParentHistory.empty()) {
        backToHome();
        return;
    }

    const auto previous = m_mediaParentHistory.back();
    m_mediaParentHistory.pop_back();
    resetMediaDirectory(previous.first, previous.second);
    loadMediaDirectory(true);
}

void AppViewModel::mediaDetailsBack()
{
    if (!m_currentLibrary) {
        backToHome();
        return;
    }

    m_selectedItem.reset();
    clearSeriesDetails();
    syncSelectedPeople();
    clearCurrentPlayback();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("library"));
}

void AppViewModel::openSettings()
{
    clearSeriesDetails();
    clearCurrentPlayback();
    emit playbackChanged();
    setCurrentView(QStringLiteral("settings"));
}

void AppViewModel::openHistoryStats()
{
    clearSeriesDetails();
    clearCurrentPlayback();
    flushPendingUsageStats(false);
    refreshUsageStats();
    emit playbackChanged();
    setCurrentView(QStringLiteral("history"));
}

void AppViewModel::refreshHistoryStats()
{
    flushPendingUsageStats(false);
    refreshUsageStats();
}

void AppViewModel::openScheduledPlaybackTasks()
{
    clearError();
    refreshScheduledEmbySources();
    refreshScheduledPlaybackTasks();
    setCurrentView(QStringLiteral("scheduledTasks"));
}

void AppViewModel::beginAddScheduledPlaybackTask()
{
    m_scheduledTaskEditingId.clear();
    setScheduledTaskSourceIndex(m_scheduledEmbySources.count() > 0 ? 0 : -1);
    setScheduledTaskDurationMinutes(90);
    emit scheduledTaskEditorChanged();
}

void AppViewModel::editScheduledPlaybackTask(int row)
{
    const auto task = m_scheduledPlaybackTasks.taskAt(row);
    if (!task) {
        return;
    }

    m_scheduledTaskEditingId = task->id;
    auto sourceIndex = -1;
    for (auto index = 0; index < m_scheduledEmbySources.count(); ++index) {
        const auto card = m_scheduledEmbySources.cardAt(index);
        if (card && card->server.id == task->serverId) {
            sourceIndex = index;
            break;
        }
    }
    m_scheduledTaskSourceIndex = sourceIndex;
    m_scheduledTaskDurationMinutes = task->durationMinutes;
    emit scheduledTaskEditorChanged();
}

bool AppViewModel::saveAndRunScheduledPlaybackTask()
{
    clearError();
    if (scheduledPlaybackActive() || scheduledPlaybackWaiting()) {
        setError(trText(QStringLiteral("schedule.errorBusy")));
        return false;
    }
    const auto source = m_scheduledEmbySources.cardAt(m_scheduledTaskSourceIndex);
    if (!source || source->server.serviceType != ServiceType::Emby || !source->hasSession) {
        setError(trText(QStringLiteral("schedule.errorSource")));
        return false;
    }

    const ScheduledPlaybackTask task {
        .id = m_scheduledTaskEditingId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : m_scheduledTaskEditingId,
        .serverId = source->server.id,
        .serverName = source->server.name,
        .username = source->server.username,
        .startTime = QStringLiteral("manual"),
        .durationMinutes = std::clamp(m_scheduledTaskDurationMinutes, 5, 720),
        .enabled = true,
        .lastRunDate = QStringLiteral(""),
        .privateMode = source->server.privateMode,
    };
    if (auto result = m_repository.saveScheduledPlaybackTask(task); !result) {
        setError(result.error());
        return false;
    }

    refreshScheduledPlaybackTasks();
    m_scheduledPlaybackManager.runNow(task);
    return true;
}

void AppViewModel::deleteScheduledPlaybackTask(int row)
{
    clearError();
    const auto task = m_scheduledPlaybackTasks.taskAt(row);
    if (!task) {
        return;
    }
    if (scheduledPlaybackActive() || scheduledPlaybackWaiting()) {
        m_scheduledPlaybackManager.stop();
    }
    if (auto result = m_repository.deleteScheduledPlaybackTask(task->id); !result) {
        setError(result.error());
        return;
    }
    refreshScheduledPlaybackTasks();
}

void AppViewModel::runScheduledPlaybackTaskNow(int row)
{
    clearError();
    if (scheduledPlaybackActive() || scheduledPlaybackWaiting()) {
        setError(trText(QStringLiteral("schedule.errorBusy")));
        return;
    }
    const auto task = m_scheduledPlaybackTasks.taskAt(row);
    if (!task) {
        return;
    }
    m_scheduledPlaybackManager.runNow(*task);
}

void AppViewModel::stopScheduledPlayback()
{
    m_scheduledPlaybackManager.stop();
}

QString AppViewModel::formatDuration(qint64 seconds) const
{
    const auto normalized = std::max<qint64>(0, seconds);
    const auto hours = normalized / 3600;
    const auto minutes = (normalized % 3600) / 60;
    const auto remainingSeconds = normalized % 60;
    if (hours > 0) {
        return effectiveLanguage(m_languageMode) == QStringLiteral("zh_CN")
            ? QStringLiteral("%1 小时 %2 分钟").arg(hours).arg(minutes)
            : QStringLiteral("%1h %2m").arg(hours).arg(minutes);
    }
    if (minutes > 0) {
        return effectiveLanguage(m_languageMode) == QStringLiteral("zh_CN")
            ? QStringLiteral("%1 分钟 %2 秒").arg(minutes).arg(remainingSeconds)
            : QStringLiteral("%1m %2s").arg(minutes).arg(remainingSeconds);
    }
    return effectiveLanguage(m_languageMode) == QStringLiteral("zh_CN")
        ? QStringLiteral("%1 秒").arg(remainingSeconds)
        : QStringLiteral("%1s").arg(remainingSeconds);
}

void AppViewModel::refreshHome()
{
    if (!m_session) {
        return;
    }
    refreshContinueWatching();
    refreshLibraries();
}

void AppViewModel::refreshLibraries()
{
    if (!m_session) {
        return;
    }

    clearError();
    m_currentLibrary.reset();
    clearMediaDirectoryState();
    m_items.clear();
    emit currentLibraryChanged();

    auto* client = clientFor(m_session->server.serviceType);
    beginHomeLoading();
    setLoading(true);
    AppLogger::info(QStringLiteral("library"),
                    QStringLiteral("Fetching libraries from %1").arg(QUrl(m_session->server.baseUrl).host()));
    client->fetchLibraries(*m_session, [this](LibraryResult result) {
        endHomeLoading();
        setLoading(false);
        if (!result) {
            AppLogger::warning(QStringLiteral("library"), QStringLiteral("Fetch libraries failed: %1").arg(displayNetworkError(result.error())));
            setError(displayNetworkError(result.error()));
            return;
        }
        AppLogger::info(QStringLiteral("library"), QStringLiteral("Fetched %1 libraries").arg(result->size()));
        m_libraries.setLibraries(std::move(*result));
    });
}

void AppViewModel::refreshContinueWatching()
{
    if (!m_session) {
        return;
    }

    auto* client = clientFor(m_session->server.serviceType);
    beginHomeLoading();
    AppLogger::info(QStringLiteral("continue"), QStringLiteral("Fetching resume items from %1").arg(QUrl(m_session->server.baseUrl).host()));
    client->fetchContinueWatching(*m_session, 24, [this](ItemResult result) {
        endHomeLoading();
        if (!result) {
            AppLogger::warning(QStringLiteral("continue"), QStringLiteral("Fetch resume items failed: %1").arg(displayNetworkError(result.error())));
            setError(displayNetworkError(result.error()));
            return;
        }
        auto items = std::move(*result);
        mergeRecentPlaybackProgress(items);
        m_continueItems.setItems(std::move(items));
    });
}

void AppViewModel::resetMediaDirectory(const QString& id, const QString& name)
{
    const auto changed = m_currentMediaParentId != id || m_currentMediaParentName != name;
    m_currentMediaParentId = id;
    m_currentMediaParentName = name;
    m_nextItemStartIndex = 0;
    m_hasMoreMediaItems = true;
    if (changed) {
        emit currentLibraryChanged();
    }
}

void AppViewModel::clearMediaDirectoryState()
{
    resetMediaDirectory({}, {});
    m_mediaParentHistory.clear();
}

void AppViewModel::loadMediaDirectory(bool resetItems)
{
    if (!m_session || !m_currentLibrary || m_loading) {
        return;
    }

    if (m_currentMediaParentId.isEmpty()) {
        resetMediaDirectory(m_currentLibrary->id, m_currentLibrary->name);
    }

    if (resetItems) {
        m_nextItemStartIndex = 0;
        m_hasMoreMediaItems = true;
        m_items.clear();
        m_selectedItem.reset();
        clearSeriesDetails();
        syncSelectedPeople();
        emit selectedItemChanged();
    } else if (!m_hasMoreMediaItems) {
        return;
    }

    const auto requestParentId = m_currentMediaParentId;
    auto* client = clientFor(m_session->server.serviceType);
    setLibraryItemsLoading(true);
    setLoading(true);
    AppLogger::info(QStringLiteral("items"),
                    QStringLiteral("Fetching items for parent %1").arg(requestParentId));
    client->fetchLibraryItems(*m_session, *m_currentLibrary, requestParentId, m_nextItemStartIndex, m_itemPageSize, [this, requestParentId](ItemResult result) {
        setLibraryItemsLoading(false);
        setLoading(false);
        if (requestParentId != m_currentMediaParentId) {
            AppLogger::info(QStringLiteral("items"), QStringLiteral("Ignoring stale item page for parent %1").arg(requestParentId));
            return;
        }
        if (!result) {
            AppLogger::warning(QStringLiteral("items"), QStringLiteral("Fetch items failed: %1").arg(displayNetworkError(result.error())));
            setError(displayNetworkError(result.error()));
            return;
        }

        const auto count = static_cast<int>(result->size());
        m_nextItemStartIndex += count;
        const auto appendedCount = m_items.appendItems(std::move(*result));
        m_hasMoreMediaItems = count >= m_itemPageSize && appendedCount > 0;
        AppLogger::info(QStringLiteral("items"),
                        QStringLiteral("Fetched %1 items, appended %2 unique items").arg(count).arg(appendedCount));
        setCurrentView(QStringLiteral("library"));
    });
}

bool AppViewModel::isNavigableMediaFolder(const MediaItem& item) const
{
    return item.folder
        || item.itemType.compare(QStringLiteral("Folder"), Qt::CaseInsensitive) == 0
        || item.itemType.compare(QStringLiteral("BoxSet"), Qt::CaseInsensitive) == 0
        || item.itemType.compare(QStringLiteral("CollectionFolder"), Qt::CaseInsensitive) == 0;
}

void AppViewModel::clearSeriesDetails()
{
    ++m_seriesRequestGeneration;
    m_selectedSeason.reset();
    m_seriesSeasons.clear();
    m_seriesEpisodes.clear();
    emit selectedSeasonChanged();
}

void AppViewModel::loadSeriesSeasons()
{
    if (!m_session || !m_selectedItem || !selectedItemHasSeriesEpisodes()) {
        clearSeriesDetails();
        return;
    }

    auto* client = clientFor(m_session->server.serviceType);
    if (!client) {
        clearSeriesDetails();
        return;
    }

    const auto seriesId = m_selectedItem->seriesId.isEmpty() ? m_selectedItem->id : m_selectedItem->seriesId;
    const auto currentSeasonId = isEpisodeItem(*m_selectedItem) ? m_selectedItem->parentId : QString {};
    const auto currentSeasonNumber = isEpisodeItem(*m_selectedItem) ? m_selectedItem->parentIndexNumber : QString {};
    const auto generation = ++m_seriesRequestGeneration;
    m_selectedSeason.reset();
    m_seriesSeasons.clear();
    m_seriesEpisodes.clear();
    emit selectedSeasonChanged();

    AppLogger::info(QStringLiteral("series"), QStringLiteral("Fetching seasons for selected series"));
    client->fetchSeriesSeasons(*m_session,
                               seriesId,
                               [this, generation, currentSeasonId, currentSeasonNumber](ItemResult result) {
        if (generation != m_seriesRequestGeneration) {
            return;
        }
        if (!result) {
            AppLogger::warning(QStringLiteral("series"), QStringLiteral("Fetch seasons failed: %1").arg(displayNetworkError(result.error())));
            setError(displayNetworkError(result.error()));
            return;
        }

        auto seasons = std::move(*result);
        std::ranges::sort(seasons, [](const MediaItem& left, const MediaItem& right) {
            const auto leftIndex = left.indexNumber.toInt();
            const auto rightIndex = right.indexNumber.toInt();
            if (leftIndex != rightIndex) {
                return leftIndex < rightIndex;
            }
            return left.name.localeAwareCompare(right.name) < 0;
        });
        const auto count = static_cast<int>(seasons.size());
        AppLogger::info(QStringLiteral("series"), QStringLiteral("Fetched %1 seasons").arg(count));
        m_seriesSeasons.setItems(std::move(seasons));
        if (count > 0) {
            auto selectedRow = 0;
            for (auto row = 0; row < m_seriesSeasons.count(); ++row) {
                const auto season = m_seriesSeasons.itemAt(row);
                if (!season) {
                    continue;
                }
                const auto idMatches = !currentSeasonId.isEmpty() && season->id == currentSeasonId;
                const auto numberMatches = !currentSeasonNumber.isEmpty() && season->indexNumber == currentSeasonNumber;
                if (idMatches || numberMatches) {
                    selectedRow = row;
                    break;
                }
            }
            selectSeason(selectedRow);
        }
    });
}

void AppViewModel::loadSeasonEpisodes(const MediaItem& season)
{
    if (!m_session || !m_selectedItem || !selectedItemHasSeriesEpisodes() || season.id.isEmpty()) {
        m_seriesEpisodes.clear();
        return;
    }

    auto* client = clientFor(m_session->server.serviceType);
    if (!client) {
        m_seriesEpisodes.clear();
        return;
    }

    const auto seriesId = m_selectedItem->seriesId.isEmpty() ? m_selectedItem->id : m_selectedItem->seriesId;
    const auto seasonId = season.id;
    const auto generation = m_seriesRequestGeneration;

    m_seriesEpisodes.clear();
    AppLogger::info(QStringLiteral("series"), QStringLiteral("Fetching episodes for selected season"));
    client->fetchSeasonEpisodes(*m_session, seriesId, seasonId, [this, generation](ItemResult result) {
        if (generation != m_seriesRequestGeneration) {
            return;
        }
        if (!result) {
            AppLogger::warning(QStringLiteral("series"), QStringLiteral("Fetch episodes failed: %1").arg(displayNetworkError(result.error())));
            setError(displayNetworkError(result.error()));
            return;
        }

        auto episodes = std::move(*result);
        std::ranges::sort(episodes, [](const MediaItem& left, const MediaItem& right) {
            const auto leftSeason = left.parentIndexNumber.toInt();
            const auto rightSeason = right.parentIndexNumber.toInt();
            if (leftSeason != rightSeason) {
                return leftSeason < rightSeason;
            }
            const auto leftEpisode = left.indexNumber.toInt();
            const auto rightEpisode = right.indexNumber.toInt();
            if (leftEpisode != rightEpisode) {
                return leftEpisode < rightEpisode;
            }
            return left.name.localeAwareCompare(right.name) < 0;
        });
        AppLogger::info(QStringLiteral("series"), QStringLiteral("Fetched %1 episodes").arg(static_cast<int>(episodes.size())));
        m_seriesEpisodes.setItems(std::move(episodes));
    });
}

void AppViewModel::clearCurrentPlayback(double stopPositionSeconds)
{
    if (stopPositionSeconds >= 0.0) {
        reportPlaybackStopped(stopPositionSeconds);
    } else {
        finishPlaybackUsageTracking();
    }
    m_currentPlaybackUrl = QUrl();
    m_currentIptvChannelId.clear();
    m_currentMediaSourceId.clear();
    m_currentPlaySessionId.clear();
    m_playbackHttpUsername.clear();
    m_playbackHttpPassword.clear();
    m_playbackAllowInsecureTls = false;
    if (!m_webDavPlaybackStreamId.isEmpty()) {
        m_webDavPlaybackProxy.revoke(m_webDavPlaybackStreamId);
        m_webDavPlaybackStreamId.clear();
    }
    m_currentPlaybackStartSeconds = 0.0;
    m_lastPlaybackReportSeconds = -1.0;
    m_playbackStartedReported = false;
    setForegroundPlaybackActive(false);
}

void AppViewModel::syncSelectedPeople()
{
    if (m_selectedItem) {
        m_selectedPeople.setPeople(m_selectedItem->peopleList);
    } else {
        m_selectedPeople.clear();
    }
}

void AppViewModel::openLibrary(int row)
{
    if (!m_session) {
        return;
    }

    const auto library = m_libraries.libraryAt(row);
    if (!library) {
        return;
    }

    m_currentLibrary = *library;
    clearMediaDirectoryState();
    resetMediaDirectory(library->id, library->name);
    m_selectedItem.reset();
    clearSeriesDetails();
    syncSelectedPeople();
    clearCurrentPlayback();
    m_items.clear();
    m_nextItemStartIndex = 0;
    emit currentLibraryChanged();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("library"));
    loadMediaDirectory(false);
}

void AppViewModel::openContinueItem(int row)
{
    const auto item = m_continueItems.itemAt(row);
    if (!item) {
        return;
    }

    m_selectedItem = *item;
    clearSeriesDetails();
    syncSelectedPeople();
    clearCurrentPlayback();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("details"));

    if (!m_session) {
        return;
    }
    const auto resumePositionTicks = item->playbackPositionTicks;
    const auto resumePlayedPercentage = item->playedPercentage;
    auto* client = clientFor(m_session->server.serviceType);
    setLoading(true);
    client->fetchItemDetails(*m_session,
                             item->id,
                             [this, resumePositionTicks, resumePlayedPercentage](std::expected<MediaItem, NetworkError> result) {
        setLoading(false);
        if (!result) {
            setError(displayNetworkError(result.error()));
            return;
        }
        if (result->playbackPositionTicks <= 0 && resumePositionTicks > 0) {
            result->playbackPositionTicks = resumePositionTicks;
        }
        if (result->playedPercentage <= 0.0 && resumePlayedPercentage > 0.0) {
            result->playedPercentage = resumePlayedPercentage;
        }
        m_selectedItem = std::move(*result);
        syncSelectedPeople();
        emit selectedItemChanged();
        if (selectedItemHasSeriesEpisodes()) {
            loadSeriesSeasons();
        } else {
            clearSeriesDetails();
        }
    });
}

void AppViewModel::openItem(int row)
{
    const auto item = m_items.itemAt(row);
    if (!item) {
        return;
    }

    if (isNavigableMediaFolder(*item)) {
        m_mediaParentHistory.emplace_back(m_currentMediaParentId, m_currentMediaParentName);
        resetMediaDirectory(item->id, item->name);
        loadMediaDirectory(true);
        return;
    }

    m_selectedItem = *item;
    clearSeriesDetails();
    syncSelectedPeople();
    clearCurrentPlayback();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("details"));

    if (!m_session) {
        return;
    }
    auto* client = clientFor(m_session->server.serviceType);
    setLoading(true);
    client->fetchItemDetails(*m_session, item->id, [this](std::expected<MediaItem, NetworkError> result) {
        setLoading(false);
        if (!result) {
            setError(displayNetworkError(result.error()));
            return;
        }
        m_selectedItem = std::move(*result);
        syncSelectedPeople();
        emit selectedItemChanged();
        if (selectedItemHasSeriesEpisodes()) {
            loadSeriesSeasons();
        } else {
            clearSeriesDetails();
        }
    });
}

void AppViewModel::selectSeason(int row)
{
    const auto season = m_seriesSeasons.itemAt(row);
    if (!season) {
        return;
    }

    ++m_seriesRequestGeneration;
    m_selectedSeason = *season;
    emit selectedSeasonChanged();
    loadSeasonEpisodes(*season);
}

void AppViewModel::openEpisode(int row)
{
    const auto item = m_seriesEpisodes.itemAt(row);
    if (!item) {
        return;
    }

    auto episodeContext = *item;
    if (m_selectedItem) {
        applyMissingEpisodeContext(episodeContext, seriesContextFor(*m_selectedItem));
    }
    if (m_selectedSeason) {
        episodeContext.parentId = m_selectedSeason->id;
        episodeContext.parentIndexNumber = m_selectedSeason->indexNumber;
        episodeContext.seasonName = m_selectedSeason->name;
    }

    m_selectedItem = episodeContext;
    clearSeriesDetails();
    syncSelectedPeople();
    clearCurrentPlayback();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("details"));

    if (!m_session) {
        return;
    }
    auto* client = clientFor(m_session->server.serviceType);
    setLoading(true);
    client->fetchItemDetails(*m_session, episodeContext.id, [this, episodeContext](std::expected<MediaItem, NetworkError> result) {
        setLoading(false);
        if (!result) {
            setError(displayNetworkError(result.error()));
            return;
        }
        auto detail = std::move(*result);
        applyMissingEpisodeContext(detail, episodeContext);
        m_selectedItem = std::move(detail);
        syncSelectedPeople();
        emit selectedItemChanged();
        if (selectedItemHasSeriesEpisodes()) {
            loadSeriesSeasons();
        } else {
            clearSeriesDetails();
        }
    });
}

void AppViewModel::playSelectedItem()
{
    if (!m_session || !m_selectedItem) {
        setError(QStringLiteral("Select a playable media item first"));
        return;
    }
    if (selectedItemIsSeries()) {
        setError(QStringLiteral("Select an episode to play"));
        return;
    }

    auto* client = clientFor(m_session->server.serviceType);
    if (!client) {
        setError(QStringLiteral("Unsupported server type"));
        return;
    }

    clearError();
    setForegroundPlaybackActive(true);
    setLoading(true);
    const auto itemName = m_selectedItem->name;
    AppLogger::info(QStringLiteral("player"), QStringLiteral("Fetching playback info for selected media item"));
    client->fetchPlaybackUrl(*m_session, *m_selectedItem, [this, itemName](PlaybackUrlResult result) {
        setLoading(false);
        if (!result) {
            setForegroundPlaybackActive(false);
            const auto message = displayNetworkError(result.error());
            AppLogger::warning(QStringLiteral("player"), QStringLiteral("Fetch playback URL failed for %1: %2").arg(itemName, message));
            setError(message);
            return;
        }

        m_currentPlaybackUrl = result->url;
        m_currentIptvChannelId.clear();
        m_currentMediaSourceId = result->mediaSourceId;
        m_currentPlaySessionId = result->playSessionId;
        m_currentPlaybackStartSeconds = result->startSeconds;
        m_lastPlaybackReportSeconds = -1.0;
        m_playbackStartedReported = false;
        AppLogger::info(QStringLiteral("player"), QStringLiteral("Opening player for selected media item"));
        setCurrentView(QStringLiteral("player"));
        emit playbackChanged();
    });
}

void AppViewModel::reportPlaybackStarted()
{
    beginPlaybackUsageTracking();
    if (!m_session || !m_selectedItem || m_playbackStartedReported) {
        return;
    }
    auto* client = clientFor(m_session->server.serviceType);
    if (!client) {
        return;
    }

    const PlaybackReport report {
        .itemId = m_selectedItem->id,
        .mediaSourceId = m_currentMediaSourceId,
        .playSessionId = m_currentPlaySessionId,
        .positionTicks = std::max<qint64>(0, m_selectedItem->playbackPositionTicks),
        .paused = false,
    };
    client->reportPlaybackStart(*m_session, report);
    m_playbackStartedReported = true;
    AppLogger::info(QStringLiteral("player"), QStringLiteral("Reported playback start"));
}

void AppViewModel::reportPlaybackProgress(double positionSeconds, bool paused)
{
    if (!m_playbackUsageActive) {
        beginPlaybackUsageTracking();
    }
    if (m_playbackUsageActive) {
        recordPlaybackUsageUntilNow();
        m_playbackUsagePaused = paused;
    }

    if (!m_session || !m_selectedItem || !m_playbackStartedReported) {
        return;
    }
    if (positionSeconds < 0 || (!paused && m_lastPlaybackReportSeconds >= 0 && std::abs(positionSeconds - m_lastPlaybackReportSeconds) < 10.0)) {
        return;
    }

    auto* client = clientFor(m_session->server.serviceType);
    if (!client) {
        return;
    }

    const auto ticks = static_cast<qint64>(std::max(0.0, positionSeconds) * static_cast<double>(playbackTicksPerSecond));
    const PlaybackReport report {
        .itemId = m_selectedItem->id,
        .mediaSourceId = m_currentMediaSourceId,
        .playSessionId = m_currentPlaySessionId,
        .positionTicks = ticks,
        .paused = paused,
    };
    client->reportPlaybackProgress(*m_session, report);
    applyReportedPlaybackProgress(report.itemId, report.positionTicks);
    m_lastPlaybackReportSeconds = positionSeconds;
}

void AppViewModel::reportPlaybackStopped(double positionSeconds)
{
    finishPlaybackUsageTracking();
    if (!m_session || !m_selectedItem || !m_playbackStartedReported) {
        return;
    }

    auto* client = clientFor(m_session->server.serviceType);
    if (!client) {
        return;
    }

    const auto ticks = static_cast<qint64>(std::max(0.0, positionSeconds) * static_cast<double>(playbackTicksPerSecond));
    const PlaybackReport report {
        .itemId = m_selectedItem->id,
        .mediaSourceId = m_currentMediaSourceId,
        .playSessionId = m_currentPlaySessionId,
        .positionTicks = ticks,
        .paused = false,
    };
    client->reportPlaybackStopped(*m_session, report);
    applyReportedPlaybackProgress(report.itemId, report.positionTicks);
    m_playbackStartedReported = false;
    m_lastPlaybackReportSeconds = -1.0;
    QTimer::singleShot(continueRefreshAfterStopMs, this, [this] {
        if (m_session) {
            refreshContinueWatching();
        }
    });
    AppLogger::info(QStringLiteral("player"), QStringLiteral("Reported playback stop"));
}

void AppViewModel::reportPlaybackError(const QString& message)
{
    const auto normalized = message.trimmed();
    if (normalized.isEmpty()) {
        return;
    }

    AppLogger::warning(QStringLiteral("player"), normalized);
    setError(normalized);
}

void AppViewModel::closePlayerToDetails()
{
    clearCurrentPlayback();
    emit playbackChanged();
    if (m_currentWebDavCard) {
        setCurrentView(QStringLiteral("webdav"));
        return;
    }
    if (m_currentIptvCard) {
        setCurrentView(QStringLiteral("iptv"));
        return;
    }
    setCurrentView(m_selectedItem ? QStringLiteral("details") : QStringLiteral("home"));
}

void AppViewModel::loadMoreItems()
{
    loadMediaDirectory(false);
}

void AppViewModel::clearError()
{
    if (m_errorMessage.isEmpty()) {
        return;
    }
    m_errorMessage.clear();
    emit errorMessageChanged();
}

void AppViewModel::acceptPendingCertificate(bool accepted)
{
    if (m_pendingCertificateReply) {
        auto reply = std::move(m_pendingCertificateReply);
        m_pendingCertificateReply = {};
        reply(accepted);
    }
}

void AppViewModel::openLocalPlaybackForVerification(const QUrl& url)
{
    if (url.isEmpty()) {
        return;
    }

    clearError();
    setForegroundPlaybackActive(true);
    m_currentPlaybackUrl = url;
    m_currentIptvChannelId.clear();
    m_currentMediaSourceId.clear();
    m_currentPlaySessionId.clear();
    m_currentPlaybackStartSeconds = 0.0;
    m_lastPlaybackReportSeconds = -1.0;
    m_playbackStartedReported = true;

    MediaItem item;
    item.id = QStringLiteral("local-verification");
    item.name = QStringLiteral("Local Playback Verification");
    item.itemType = QStringLiteral("Video");
    m_selectedItem = std::move(item);
    clearSeriesDetails();
    syncSelectedPeople();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("player"));
    AppLogger::info(QStringLiteral("player"), QStringLiteral("Opening local playback verification media"));
}

MediaServiceClient* AppViewModel::clientFor(ServiceType type)
{
    switch (type) {
    case ServiceType::Emby:
        return &m_embyClient;
    case ServiceType::Jellyfin:
        return &m_jellyfinClient;
    case ServiceType::IPTV:
        return nullptr;
    case ServiceType::WebDAV:
        return nullptr;
    }
    return nullptr;
}

ServerConfig AppViewModel::makeServerConfig() const
{
    const auto privateMode = m_pendingServiceCard ? m_pendingServiceCard->server.privateMode : m_privacyMode;
    if (m_serviceType == ServiceType::IPTV) {
        const QFileInfo fileInfo(m_iptvFilePath);
        const auto sourcePath = fileInfo.exists() ? fileInfo.absoluteFilePath() : m_iptvFilePath.trimmed();
        const auto name = m_serverName.trimmed().isEmpty()
            ? (fileInfo.completeBaseName().isEmpty() ? QStringLiteral("IPTV") : fileInfo.completeBaseName())
            : m_serverName.trimmed();
        return ServerConfig {
            .id = iptvServiceIdFor(sourcePath),
            .name = name,
            .baseUrl = sourcePath,
            .username = QStringLiteral(""),
            .serviceType = ServiceType::IPTV,
            .trustSelfSignedCertificate = false,
            .autoLogin = true,
            .privateMode = privateMode,
        };
    }
    if (m_serviceType == ServiceType::WebDAV) {
        const auto endpoint = ensureDirectoryUrl(QUrl(m_serverUrl.trimmed())).toString();
        const auto user = m_username.trimmed();
        return ServerConfig {
            .id = cardIdFor(endpoint, m_serviceType, user),
            .name = m_serverName.trimmed().isEmpty() ? QUrl(endpoint).host() : m_serverName.trimmed(),
            .baseUrl = endpoint,
            .username = user,
            .serviceType = m_serviceType,
            .trustSelfSignedCertificate = m_trustSelfSignedCertificate,
            .autoLogin = m_autoLogin,
            .privateMode = privateMode,
        };
    }

    const auto baseUrl = m_serverUrl.trimmed();
    const auto user = m_username.trimmed();
    return ServerConfig {
        .id = cardIdFor(baseUrl, m_serviceType, user),
        .name = m_serverName.trimmed().isEmpty() ? QUrl(baseUrl).host() : m_serverName.trimmed(),
        .baseUrl = baseUrl,
        .username = user,
        .serviceType = m_serviceType,
        .trustSelfSignedCertificate = m_trustSelfSignedCertificate,
        .autoLogin = m_autoLogin,
        .privateMode = privateMode,
    };
}

void AppViewModel::refreshServiceCards()
{
    const auto cardsResult = m_repository.loadServiceCards(m_privacyMode);
    if (!cardsResult) {
        setError(cardsResult.error());
        return;
    }
    m_services.setCards(*cardsResult);
    refreshScheduledEmbySources();
}

void AppViewModel::refreshScheduledPlaybackTasks()
{
    const auto result = m_repository.loadScheduledPlaybackTasks(m_privacyMode);
    if (!result) {
        AppLogger::warning(QStringLiteral("scheduled-playback"),
                           QStringLiteral("Load scheduled tasks failed: %1").arg(result.error()));
        return;
    }
    m_scheduledPlaybackTasks.setTasks(*result);
    emit scheduledPlaybackTasksChanged();
}

void AppViewModel::refreshScheduledEmbySources()
{
    const auto result = m_repository.loadAllServiceCards();
    if (!result) {
        AppLogger::warning(QStringLiteral("scheduled-playback"),
                           QStringLiteral("Load Emby sources failed: %1").arg(result.error()));
        return;
    }

    std::vector<ServiceCard> sources;
    for (const auto& card : *result) {
        if (card.server.serviceType == ServiceType::Emby && card.hasSession &&
            (m_privacyMode || !card.server.privateMode)) {
            sources.push_back(card);
        }
    }
    m_scheduledEmbySources.setCards(std::move(sources));
}

void AppViewModel::setForegroundPlaybackActive(bool active)
{
    m_scheduledPlaybackManager.setForegroundPlaybackActive(active);
}

void AppViewModel::startLogin(const ServerConfig& server, const QString& password)
{
    auto* client = clientFor(server.serviceType);
    if (!client) {
        setError(QStringLiteral("Unsupported server type"));
        return;
    }
    if (password.isEmpty()) {
        setError(QStringLiteral("Password is required"));
        return;
    }

    setLoading(true);
    AppLogger::info(QStringLiteral("auth"),
                    QStringLiteral("Login requested for %1 server %2")
                        .arg(serviceTypeToString(server.serviceType), QUrl(server.baseUrl).host()));
    client->login(server, server.username, password, [this](LoginResult result) {
        setLoading(false);
        if (!result) {
            AppLogger::warning(QStringLiteral("auth"), QStringLiteral("Login failed: %1").arg(displayNetworkError(result.error())));
            setError(displayNetworkError(result.error()));
            return;
        }

        m_password.clear();
        emit passwordChanged();
        setSession(std::move(*result));
        AppLogger::info(QStringLiteral("auth"),
                        QStringLiteral("Login succeeded for %1 server %2")
                            .arg(serviceTypeToString(m_session->server.serviceType), QUrl(m_session->server.baseUrl).host()));
        saveSession();
        refreshServiceCards();
        loadServiceHome();
    });
}

void AppViewModel::loadServiceHome()
{
    if (!m_session) {
        setCurrentView(QStringLiteral("services"));
        return;
    }

    clearIptvState();
    m_currentLibrary.reset();
    clearMediaDirectoryState();
    m_selectedItem.reset();
    clearSeriesDetails();
    syncSelectedPeople();
    m_items.clear();
    emit currentServerChanged();
    emit currentLibraryChanged();
    emit selectedItemChanged();
    setCurrentView(QStringLiteral("home"));
    refreshContinueWatching();
    refreshLibraries();
}

void AppViewModel::loadIptvService(const ServiceCard& card)
{
    clearError();
    m_session.reset();
    m_currentIptvCard = card;
    if (!m_iptvSearchText.isEmpty()) {
        m_iptvSearchText.clear();
        emit iptvSearchTextChanged();
    }
    m_currentLibrary.reset();
    clearMediaDirectoryState();
    m_selectedItem.reset();
    clearSeriesDetails();
    syncSelectedPeople();
    clearCurrentPlayback();
    m_libraries.clear();
    m_continueItems.clear();
    m_items.clear();
    emit loggedInChanged();
    emit currentUserChanged();
    emit currentServerChanged();
    emit currentLibraryChanged();
    emit selectedItemChanged();
    emit playbackChanged();

    const auto playlistResult = m_repository.loadIptvPlaylist(card.server.id);
    if (!playlistResult) {
        setError(playlistResult.error());
        return;
    }
    if (!playlistResult->has_value()) {
        setError(QStringLiteral("IPTV playlist data was not found"));
        return;
    }
    m_currentIptvPlaylist = **playlistResult;
    refreshIptvChannels();
    setCurrentView(QStringLiteral("iptv"));
}

void AppViewModel::clearIptvState()
{
    if (!m_currentIptvCard && !m_currentIptvPlaylist && m_allIptvChannels.empty() && m_iptvGroups.isEmpty() && m_currentIptvChannelId.isEmpty()) {
        return;
    }

    const auto hadIptvPlayback = !m_currentIptvChannelId.isEmpty();
    m_currentIptvCard.reset();
    m_currentIptvPlaylist.reset();
    m_currentIptvChannelId.clear();
    m_allIptvChannels.clear();
    m_iptvChannels.clear();
    m_iptvGroups.clear();
    m_iptvSelectedGroup.clear();
    emit currentUserChanged();
    emit currentServerChanged();
    emit iptvGroupsChanged();
    emit iptvSelectedGroupChanged();
    if (hadIptvPlayback) {
        emit playbackChanged();
    }
}

void AppViewModel::refreshIptvChannels()
{
    if (!m_currentIptvCard) {
        m_allIptvChannels.clear();
        m_iptvChannels.clear();
        return;
    }

    const auto channelsResult = m_repository.loadIptvChannels(m_currentIptvCard->server.id);
    if (!channelsResult) {
        setError(channelsResult.error());
        return;
    }

    m_allIptvChannels = *channelsResult;

    QStringList groups { allIptvGroup() };
    for (const auto& channel : m_allIptvChannels) {
        const auto group = channel.groupName.isEmpty() ? defaultIptvGroup() : channel.groupName;
        if (!groups.contains(group, Qt::CaseInsensitive)) {
            groups.push_back(group);
        }
    }
    std::sort(groups.begin() + 1, groups.end(), [](const QString& left, const QString& right) {
        return left.localeAwareCompare(right) < 0;
    });
    m_iptvGroups = groups;
    emit iptvGroupsChanged();

    if (!m_iptvGroups.contains(m_iptvSelectedGroup, Qt::CaseInsensitive)) {
        m_iptvSelectedGroup = allIptvGroup();
        emit iptvSelectedGroupChanged();
    }
    applyIptvFilters();
}

void AppViewModel::applyIptvFilters()
{
    std::vector<IptvChannel> filtered;
    const auto search = m_iptvSearchText.trimmed();
    const auto selectedGroup = m_iptvSelectedGroup.isEmpty() ? allIptvGroup() : m_iptvSelectedGroup;

    for (const auto& channel : m_allIptvChannels) {
        const auto group = channel.groupName.isEmpty() ? defaultIptvGroup() : channel.groupName;
        const auto groupMatches = selectedGroup == allIptvGroup() || group.compare(selectedGroup, Qt::CaseInsensitive) == 0;
        const auto searchMatches = search.isEmpty()
            || channel.name.contains(search, Qt::CaseInsensitive)
            || group.contains(search, Qt::CaseInsensitive);
        if (groupMatches && searchMatches) {
            filtered.push_back(channel);
        }
    }

    m_iptvChannels.setChannels(std::move(filtered));
}

std::expected<IptvPlaylist, QString> AppViewModel::importIptvPlaylistFile(const ServerConfig& server,
                                                                         std::vector<IptvChannel>& channels) const
{
    const QFileInfo sourceInfo(m_iptvFilePath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        return std::unexpected(QStringLiteral("Selected IPTV playlist file does not exist"));
    }

    const auto extension = sourceInfo.suffix().toLower();
    if (extension != QStringLiteral("m3u") && extension != QStringLiteral("m3u8")) {
        return std::unexpected(QStringLiteral("Select an M3U or M3U8 playlist file"));
    }

    const auto playlistId = iptvPlaylistIdFor(server.id);
    const auto targetName = QStringLiteral("%1.%2").arg(safeFileName(server.id), extension);
    const auto targetPath = QDir(iptvImportDirectory()).filePath(targetName);

    if (QFileInfo(targetPath).exists() && !QFile::remove(targetPath)) {
        return std::unexpected(QStringLiteral("Unable to replace previous IPTV playlist copy"));
    }
    if (!QFile::copy(sourceInfo.absoluteFilePath(), targetPath)) {
        return std::unexpected(QStringLiteral("Unable to import IPTV playlist file"));
    }

    for (auto& channel : channels) {
        channel.playlistId = playlistId;
        if (channel.streamUrl == QUrl::fromLocalFile(sourceInfo.absoluteFilePath()).toString()) {
            channel.streamUrl = QUrl::fromLocalFile(targetPath).toString();
        }
    }

    return IptvPlaylist {
        .id = playlistId,
        .serviceId = server.id,
        .name = server.name,
        .sourceType = QStringLiteral("LocalFile"),
        .sourcePath = sourceInfo.absoluteFilePath(),
        .importedPath = targetPath,
        .importedAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODate),
    };
}

void AppViewModel::loadWebDavService(const ServiceCard& card, const QString& password)
{
    clearError();
    m_session.reset();
    clearIptvState();
    m_currentWebDavCard = card;
    m_webDavPassword = password;
    m_webDavHistory.clear();
    m_currentLibrary.reset();
    clearMediaDirectoryState();
    m_selectedItem.reset();
    clearSeriesDetails();
    syncSelectedPeople();
    clearCurrentPlayback();
    m_libraries.clear();
    m_continueItems.clear();
    m_items.clear();
    emit loggedInChanged();
    emit currentUserChanged();
    emit currentServerChanged();
    emit currentLibraryChanged();
    emit selectedItemChanged();
    emit playbackChanged();
    loadWebDavDirectory(ensureDirectoryUrl(QUrl(card.server.baseUrl)));
}

void AppViewModel::clearWebDavState()
{
    if (!m_currentWebDavCard && m_webDavCurrentUrl.isEmpty() && m_webDavItems.count() == 0) {
        return;
    }
    m_currentWebDavCard.reset();
    m_webDavPassword.clear();
    m_webDavCurrentUrl = QUrl();
    m_webDavHistory.clear();
    m_webDavItems.clear();
    emit currentServerChanged();
    emit webDavCurrentPathChanged();
}

void AppViewModel::loadWebDavDirectory(const QUrl& url)
{
    if (!m_currentWebDavCard) {
        return;
    }
    const auto directoryUrl = ensureDirectoryUrl(url);
    setLoading(true);
    m_webDavClient.listDirectory(m_currentWebDavCard->server, m_webDavPassword, directoryUrl, [this, directoryUrl](WebDavListResult result) {
        setLoading(false);
        if (!result) {
            setError(displayNetworkError(result.error()));
            return;
        }
        m_webDavCurrentUrl = directoryUrl;
        m_webDavItems.setItems(std::move(*result));
        emit webDavCurrentPathChanged();
        setCurrentView(QStringLiteral("webdav"));
    });
}

void AppViewModel::saveWebDavCredentials(const ServerConfig& server, const QString& password)
{
    if (password.isEmpty() || !server.autoLogin) {
        return;
    }
    if (!CredentialStore::isAvailable()) {
        setError(QStringLiteral("System credential store is unavailable; WebDAV password will be requested when opening this service"));
        return;
    }
    if (auto result = CredentialStore::savePassword(server.id, server.username, password); !result) {
        setError(result.error());
    }
}

std::optional<QString> AppViewModel::loadWebDavPassword(const ServerConfig& server)
{
    if (!CredentialStore::isAvailable()) {
        return std::nullopt;
    }
    auto password = CredentialStore::loadPassword(server.id);
    if (!password) {
        setError(password.error());
        return std::nullopt;
    }
    return *password;
}

QUrl AppViewModel::childWebDavUrl(const QString& name, bool directory) const
{
    auto encoded = QString::fromUtf8(QUrl::toPercentEncoding(name));
    if (directory && !encoded.endsWith(QLatin1Char('/'))) {
        encoded.append(QLatin1Char('/'));
    }
    return ensureDirectoryUrl(m_webDavCurrentUrl).resolved(QUrl(encoded));
}

QString AppViewModel::uniqueLocalPath(const QString& directory, const QString& name) const
{
    const QFileInfo original(QDir(directory).filePath(name));
    if (!original.exists()) {
        return original.absoluteFilePath();
    }

    const auto base = original.completeBaseName();
    const auto suffix = original.suffix();
    for (int index = 1; index < 10000; ++index) {
        const auto candidateName = suffix.isEmpty()
            ? QStringLiteral("%1 (%2)").arg(base).arg(index)
            : QStringLiteral("%1 (%2).%3").arg(base).arg(index).arg(suffix);
        const QFileInfo candidate(QDir(directory).filePath(candidateName));
        if (!candidate.exists()) {
            return candidate.absoluteFilePath();
        }
    }
    return original.absoluteFilePath();
}

void AppViewModel::enqueueWebDavUploadFile(const QString& localPath, const QUrl& remoteUrl)
{
    if (!m_currentWebDavCard) {
        return;
    }
    const QFileInfo info(localPath);
    if (!info.exists() || !info.isFile()) {
        return;
    }
    m_transferManager.enqueueUpload(m_currentWebDavCard->server,
                                    m_webDavPassword,
                                    info.absoluteFilePath(),
                                    remoteUrl,
                                    info.size());
}

void AppViewModel::wireWebDavCertificatePrompt()
{
    connect(&m_webDavClient,
            &WebDavClient::certificateConfirmationRequired,
            this,
            [this](const QString& host, const QList<QSslError>& errors, std::function<void(bool)> reply) {
                QStringList details;
                for (const auto& error : errors) {
                    details.push_back(error.errorString());
                }
                m_pendingCertificateReply = std::move(reply);
                emit certificatePromptRequested(host, details.join(QLatin1Char('\n')));
            });
    connect(&m_transferManager,
            &TransferManager::certificateConfirmationRequired,
            this,
            [this](const QString& host, const QList<QSslError>& errors, std::function<void(bool)> reply) {
                QStringList details;
                for (const auto& error : errors) {
                    details.push_back(error.errorString());
                }
                m_pendingCertificateReply = std::move(reply);
                emit certificatePromptRequested(host, details.join(QLatin1Char('\n')));
            });
    connect(&m_webDavPlaybackProxy,
            &WebDavPlaybackProxy::certificateConfirmationRequired,
            this,
            [this](const QString& host, const QList<QSslError>& errors, std::function<void(bool)> reply) {
                QStringList details;
                for (const auto& error : errors) {
                    details.push_back(error.errorString());
                }
                m_pendingCertificateReply = std::move(reply);
                emit certificatePromptRequested(host, details.join(QLatin1Char('\n')));
            });
}

void AppViewModel::wireUsageSignals()
{
    connect(&m_scheduledPlaybackManager,
            &ScheduledPlaybackManager::networkTrafficSample,
            this,
            [this](const ServerConfig& server, qint64 bytesReceived) {
                if (bytesReceived <= 0) {
                    return;
                }
                const auto shouldFlush = accumulateUsage(server,
                                                         server.privateMode,
                                                         0,
                                                         bytesReceived,
                                                         0,
                                                         NetworkTrafficCategory::KeepAlive);
                const auto onHistoryPage = m_currentView == QStringLiteral("history");
                if (shouldFlush || onHistoryPage) {
                    flushPendingUsageStats(onHistoryPage);
                }
            });
    connect(&m_embyNetworkClient, &NetworkClient::networkTrafficSample, this, [this](qint64 bytesReceived, qint64 bytesSent) {
        recordNetworkUsageForCurrentService(ServiceType::Emby, bytesReceived, bytesSent);
    });
    connect(&m_jellyfinNetworkClient, &NetworkClient::networkTrafficSample, this, [this](qint64 bytesReceived, qint64 bytesSent) {
        recordNetworkUsageForCurrentService(ServiceType::Jellyfin, bytesReceived, bytesSent);
    });

    auto wireWebDavTraffic = [this](const QString& serviceId,
                                    const QString& serviceName,
                                    const QString& serviceType,
                                    qint64 bytesReceived,
                                    qint64 bytesSent) {
        ServerConfig server;
        server.id = serviceId;
        server.name = serviceName;
        server.serviceType = serviceTypeFromString(serviceType);
        if (m_currentWebDavCard && m_currentWebDavCard->server.id == serviceId) {
            server.privateMode = m_currentWebDavCard->server.privateMode;
        }
        recordNetworkUsage(server, bytesReceived, bytesSent);
    };
    connect(&m_webDavClient, &WebDavClient::networkTrafficSample, this, wireWebDavTraffic);
    connect(&m_transferManager, &TransferManager::networkTrafficSample, this, wireWebDavTraffic);
    connect(&m_webDavPlaybackProxy, &WebDavPlaybackProxy::networkTrafficSample, this, wireWebDavTraffic);
}

bool AppViewModel::accumulateUsage(const ServerConfig& server,
                                   bool privacyMode,
                                   qint64 watchSeconds,
                                   qint64 bytesReceived,
                                   qint64 bytesSent,
                                   NetworkTrafficCategory trafficCategory)
{
    if (server.id.isEmpty() || (watchSeconds <= 0 && bytesReceived <= 0 && bytesSent <= 0)) {
        return false;
    }

    const auto key = QStringLiteral("%1:%2").arg(server.id, privacyMode ? QStringLiteral("private") : QStringLiteral("normal"));
    auto& pending = m_pendingUsageStats[key];
    pending.server = server;
    pending.privacyMode = privacyMode;
    pending.watchSeconds += std::max<qint64>(0, watchSeconds);
    if (trafficCategory == NetworkTrafficCategory::KeepAlive) {
        pending.keepAliveNetworkBytesIn += std::max<qint64>(0, bytesReceived);
        pending.keepAliveNetworkBytesOut += std::max<qint64>(0, bytesSent);
    } else {
        pending.networkBytesIn += std::max<qint64>(0, bytesReceived);
        pending.networkBytesOut += std::max<qint64>(0, bytesSent);
    }

    return pending.watchSeconds >= usageWatchFlushSeconds ||
           pending.networkBytesIn + pending.networkBytesOut +
               pending.keepAliveNetworkBytesIn + pending.keepAliveNetworkBytesOut >= usageNetworkFlushBytes;
}

void AppViewModel::flushPendingUsageStats(bool refreshAfterFlush)
{
    if (m_pendingUsageStats.isEmpty()) {
        if (refreshAfterFlush) {
            refreshUsageStats();
        }
        return;
    }

    auto pendingStats = std::exchange(m_pendingUsageStats, {});
    bool wrote = false;
    for (auto it = pendingStats.cbegin(); it != pendingStats.cend(); ++it) {
        const auto& stat = it.value();
        if (auto result = m_repository.addDailyUsage(stat.server,
                                                     stat.privacyMode,
                                                     stat.watchSeconds,
                                                     stat.networkBytesIn,
                                                     stat.networkBytesOut,
                                                     stat.keepAliveNetworkBytesIn,
                                                     stat.keepAliveNetworkBytesOut);
            !result) {
            AppLogger::warning(QStringLiteral("history"), QStringLiteral("Flush usage stats failed: %1").arg(result.error()));
            auto& retry = m_pendingUsageStats[it.key()];
            retry.server = stat.server;
            retry.privacyMode = stat.privacyMode;
            retry.watchSeconds += stat.watchSeconds;
            retry.networkBytesIn += stat.networkBytesIn;
            retry.networkBytesOut += stat.networkBytesOut;
            retry.keepAliveNetworkBytesIn += stat.keepAliveNetworkBytesIn;
            retry.keepAliveNetworkBytesOut += stat.keepAliveNetworkBytesOut;
            continue;
        }
        wrote = true;
    }

    if (refreshAfterFlush && wrote) {
        refreshUsageStats();
    }
}

void AppViewModel::refreshUsageStats()
{
    const auto result = m_repository.loadDailyUsageStats(m_privacyMode);
    if (!result) {
        AppLogger::warning(QStringLiteral("history"), QStringLiteral("Load usage stats failed: %1").arg(result.error()));
        return;
    }

    qint64 watchSeconds = 0;
    qint64 normalNetworkBytes = 0;
    qint64 keepAliveNetworkBytes = 0;
    for (const auto& stat : *result) {
        watchSeconds += stat.watchSeconds;
        normalNetworkBytes += stat.networkBytesIn + stat.networkBytesOut;
        keepAliveNetworkBytes += stat.keepAliveNetworkBytesIn + stat.keepAliveNetworkBytesOut;
    }

    m_historyTotalWatchSeconds = watchSeconds;
    m_historyNormalNetworkBytes = normalNetworkBytes;
    m_historyKeepAliveNetworkBytes = keepAliveNetworkBytes;
    m_historyTotalNetworkBytes = normalNetworkBytes + keepAliveNetworkBytes;
    m_usageStats.setStats(*result);
    emit historyStatsChanged();
}

void AppViewModel::recordNetworkUsage(const ServerConfig& server, qint64 bytesReceived, qint64 bytesSent)
{
    const auto shouldFlush = accumulateUsage(server, m_privacyMode || server.privateMode, 0, bytesReceived, bytesSent);
    const auto onHistoryPage = m_currentView == QStringLiteral("history");
    if (shouldFlush || onHistoryPage) {
        flushPendingUsageStats(onHistoryPage);
    }
}

void AppViewModel::recordNetworkUsageForCurrentService(ServiceType type, qint64 bytesReceived, qint64 bytesSent)
{
    const auto server = currentServerForUsage(type);
    if (!server) {
        return;
    }
    recordNetworkUsage(*server, bytesReceived, bytesSent);
}

void AppViewModel::recordPlaybackNetworkBytes(qint64 bytesReceived)
{
    if (bytesReceived <= 0) {
        return;
    }

    const auto server = currentPlaybackServerForUsage();
    if (!server) {
        return;
    }
    if (server->serviceType == ServiceType::WebDAV) {
        return;
    }

    recordNetworkUsage(*server, bytesReceived, 0);
}

std::optional<ServerConfig> AppViewModel::currentServerForUsage(ServiceType type) const
{
    if (m_session && m_session->server.serviceType == type) {
        return m_session->server;
    }
    if (m_currentIptvCard && m_currentIptvCard->server.serviceType == type) {
        return m_currentIptvCard->server;
    }
    if (m_currentWebDavCard && m_currentWebDavCard->server.serviceType == type) {
        return m_currentWebDavCard->server;
    }
    if (m_pendingServiceCard && m_pendingServiceCard->server.serviceType == type) {
        return m_pendingServiceCard->server;
    }
    return std::nullopt;
}

std::optional<ServerConfig> AppViewModel::currentPlaybackServerForUsage() const
{
    if (m_session) {
        return m_session->server;
    }
    if (m_currentIptvCard) {
        return m_currentIptvCard->server;
    }
    if (m_currentWebDavCard) {
        return m_currentWebDavCard->server;
    }
    return std::nullopt;
}

void AppViewModel::beginPlaybackUsageTracking()
{
    if (m_playbackUsageActive) {
        return;
    }
    const auto server = currentPlaybackServerForUsage();
    if (!server) {
        return;
    }

    m_playbackUsageServer = *server;
    m_playbackUsageLastWallClock = QDateTime::currentDateTimeUtc();
    m_playbackUsagePaused = false;
    m_playbackUsageActive = true;
}

void AppViewModel::recordPlaybackUsageUntilNow()
{
    if (!m_playbackUsageActive || !m_playbackUsageServer || !m_playbackUsageLastWallClock.isValid()) {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const auto elapsedMs = m_playbackUsageLastWallClock.msecsTo(now);
    if (elapsedMs <= 0) {
        return;
    }
    if (m_playbackUsagePaused) {
        m_playbackUsageLastWallClock = now;
        return;
    }

    const auto elapsedSeconds = elapsedMs / 1000;
    if (elapsedSeconds <= 0) {
        return;
    }
    m_playbackUsageLastWallClock = m_playbackUsageLastWallClock.addSecs(elapsedSeconds);

    const auto shouldFlush = accumulateUsage(*m_playbackUsageServer, m_privacyMode || m_playbackUsageServer->privateMode, elapsedSeconds, 0, 0);
    const auto onHistoryPage = m_currentView == QStringLiteral("history");
    if (shouldFlush || onHistoryPage) {
        flushPendingUsageStats(onHistoryPage);
    }
}

void AppViewModel::finishPlaybackUsageTracking()
{
    if (!m_playbackUsageActive) {
        return;
    }
    recordPlaybackUsageUntilNow();
    m_playbackUsageActive = false;
    m_playbackUsagePaused = false;
    m_playbackUsageServer.reset();
    m_playbackUsageLastWallClock = {};
    flushPendingUsageStats(m_currentView == QStringLiteral("history"));
}

void AppViewModel::applyReportedPlaybackProgress(const QString& itemId, qint64 positionTicks)
{
    if (itemId.isEmpty()) {
        return;
    }

    const auto normalizedTicks = std::max<qint64>(0, positionTicks);
    double playedPercentage = 0.0;
    bool played = false;
    bool selectedChanged = false;

    if (m_selectedItem && m_selectedItem->id == itemId) {
        playedPercentage = playbackPercentageForTicks(*m_selectedItem, normalizedTicks);
        played = m_selectedItem->played || playedPercentage >= 99.5;
        if (m_selectedItem->playbackPositionTicks != normalizedTicks) {
            m_selectedItem->playbackPositionTicks = normalizedTicks;
            selectedChanged = true;
        }
        if (std::abs(m_selectedItem->playedPercentage - playedPercentage) > 0.01) {
            m_selectedItem->playedPercentage = playedPercentage;
            selectedChanged = true;
        }
        if (m_selectedItem->played != played) {
            m_selectedItem->played = played;
            selectedChanged = true;
        }
    }

    m_recentPlaybackProgress.insert(itemId,
                                    PlaybackProgressSnapshot {
                                        .positionTicks = normalizedTicks,
                                        .playedPercentage = playedPercentage,
                                        .played = played,
                                        .reportedAt = QDateTime::currentDateTimeUtc(),
                                    });

    m_continueItems.updatePlaybackProgress(itemId, normalizedTicks, playedPercentage, played);
    m_items.updatePlaybackProgress(itemId, normalizedTicks, playedPercentage, played);
    m_seriesEpisodes.updatePlaybackProgress(itemId, normalizedTicks, playedPercentage, played);

    if (selectedChanged) {
        emit selectedItemChanged();
    }
}

void AppViewModel::mergeRecentPlaybackProgress(std::vector<MediaItem>& items) const
{
    if (m_recentPlaybackProgress.isEmpty() || items.empty()) {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    for (auto& item : items) {
        const auto snapshot = m_recentPlaybackProgress.constFind(item.id);
        if (snapshot == m_recentPlaybackProgress.cend() || !snapshot->reportedAt.isValid()) {
            continue;
        }
        const auto ageMs = snapshot->reportedAt.msecsTo(now);
        if (ageMs < 0 || ageMs > recentPlaybackProgressMergeMs) {
            continue;
        }
        item.playbackPositionTicks = snapshot->positionTicks;
        item.playedPercentage = snapshot->playedPercentage;
        item.played = snapshot->played;
    }
}

void AppViewModel::setCurrentView(QString view)
{
    if (m_currentView == view) {
        return;
    }
    m_currentView = std::move(view);
    emit currentViewChanged();
}

void AppViewModel::setLoading(bool value)
{
    if (m_loading == value) {
        return;
    }
    m_loading = value;
    emit loadingChanged();
}

void AppViewModel::beginHomeLoading()
{
    const auto wasLoading = homeLoading();
    ++m_homeLoadingRequests;
    if (!wasLoading) {
        emit homeLoadingChanged();
    }
}

void AppViewModel::endHomeLoading()
{
    if (m_homeLoadingRequests <= 0) {
        return;
    }

    const auto wasLoading = homeLoading();
    --m_homeLoadingRequests;
    if (wasLoading && !homeLoading()) {
        emit homeLoadingChanged();
    }
}

void AppViewModel::setLibraryItemsLoading(bool value)
{
    if (m_libraryItemsLoading == value) {
        return;
    }

    m_libraryItemsLoading = value;
    emit libraryItemsLoadingChanged();
}

void AppViewModel::setError(QString message)
{
    if (m_errorMessage == message) {
        return;
    }
    m_errorMessage = std::move(message);
    emit errorMessageChanged();
}

void AppViewModel::setSession(UserSession session)
{
    m_session = std::move(session);
    emit loggedInChanged();
    emit currentUserChanged();
}

void AppViewModel::saveSession()
{
    if (!m_session) {
        return;
    }
    if (auto saveResult = m_repository.saveSession(*m_session); !saveResult) {
        setError(saveResult.error());
    }
}

bool AppViewModel::verifyPrivacyPin(const QString& pin) const
{
    const auto salt = m_repository.privacyPinSalt();
    const auto hash = m_repository.privacyPinHash();
    if (salt.isEmpty() || hash.isEmpty()) {
        return false;
    }
    return privacyPinHash(pin, salt) == hash;
}

QString AppViewModel::privacyPinHash(const QString& pin, const QString& salt) const
{
    return QString::fromLatin1(QCryptographicHash::hash((salt + QLatin1Char(':') + pin).toUtf8(), QCryptographicHash::Sha256).toHex());
}

bool AppViewModel::pinLooksValid(const QString& pin) const
{
    return validPinText(pin);
}

void AppViewModel::wireCertificatePrompt(MediaServiceClient& client)
{
    connect(&client,
            &MediaServiceClient::certificateConfirmationRequired,
            this,
            [this](const QString& host, const QList<QSslError>& errors, std::function<void(bool)> reply) {
                QStringList details;
                for (const auto& error : errors) {
                    details.push_back(error.errorString());
                }
                m_pendingCertificateReply = std::move(reply);
                emit certificatePromptRequested(host, details.join(QLatin1Char('\n')));
            });
}
