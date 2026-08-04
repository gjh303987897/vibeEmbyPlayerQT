#include "viewmodels/AppViewModel.h"

#include "services/credentials/CredentialStore.h"
#include "services/iptv/IptvParser.h"
#include "services/link/LinkPlaybackService.h"
#include "services/scheduler/ScheduledPlaybackSchedule.h"
#include "utils/AppLogger.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
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
#include <QTime>
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

ServerConfig linkPlaybackUsageServer()
{
    return ServerConfig {
        .id = QStringLiteral("builtin-link-playback"),
        .name = QStringLiteral("Link Playback"),
        .serviceType = ServiceType::Link,
    };
}

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

QString linkPlaybackErrorKey(LinkPlaybackError error)
{
    switch (error) {
    case LinkPlaybackError::Empty:
        return QStringLiteral("link.errorEmpty");
    case LinkPlaybackError::TooLong:
        return QStringLiteral("link.errorTooLong");
    case LinkPlaybackError::Invalid:
        return QStringLiteral("link.errorInvalid");
    case LinkPlaybackError::UnsupportedScheme:
        return QStringLiteral("link.errorScheme");
    case LinkPlaybackError::MissingHost:
        return QStringLiteral("link.errorHost");
    case LinkPlaybackError::EmbeddedCredentials:
        return QStringLiteral("link.errorCredentials");
    }
    return QStringLiteral("link.errorInvalid");
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

QString localMediaRootIdFor(const QString& path)
{
    auto normalized = QDir::fromNativeSeparators(QDir::cleanPath(path));
#ifdef Q_OS_WIN
    normalized = normalized.toCaseFolded();
#endif
    return QString::fromLatin1(QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256).toHex());
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
    if (item.logoImageUrl.isEmpty()) {
        item.logoImageUrl = context.logoImageUrl;
    }
    if (item.backdropImageUrls.isEmpty()) {
        item.backdropImageUrls = context.backdropImageUrls;
        item.backdropImageUrl = context.backdropImageUrl;
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
        { QStringLiteral("nav.history"), QStringLiteral("Stats") },
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
        { QStringLiteral("action.previous"), QStringLiteral("Previous") },
        { QStringLiteral("action.next"), QStringLiteral("Next") },
        { QStringLiteral("dialog.certificateTitle"), QStringLiteral("Certificate confirmation") },
        { QStringLiteral("dialog.certificatePrefix"), QStringLiteral("The server certificate for ") },
        { QStringLiteral("dialog.certificateSuffix"), QStringLiteral(" cannot be verified. Continue for this request?") },
        { QStringLiteral("dialog.passwordTitle"), QStringLiteral("Password required") },
        { QStringLiteral("dialog.serviceTitle"), QStringLiteral("Service") },
        { QStringLiteral("dialog.deleteTitle"), QStringLiteral("Delete service") },
        { QStringLiteral("dialog.deletePrompt"), QStringLiteral("Remove this service card?") },
        { QStringLiteral("dialog.deleteLocalData"), QStringLiteral("Also delete local token and cached data") },
        { QStringLiteral("dialog.exitPlaybackTitle"), QStringLiteral("Exit playback?") },
        { QStringLiteral("dialog.exitPlaybackPrompt"), QStringLiteral("Playback will stop and you will return to the previous page.") },
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
        { QStringLiteral("webdav.videoEmpty"), QStringLiteral("No folders or videos in this folder") },
        { QStringLiteral("webdav.audioEmpty"), QStringLiteral("No audio files in this folder") },
        { QStringLiteral("webdav.displayMode"), QStringLiteral("View") },
        { QStringLiteral("webdav.modeDefault"), QStringLiteral("Default") },
        { QStringLiteral("webdav.modeVideo"), QStringLiteral("Video") },
        { QStringLiteral("webdav.modeAudio"), QStringLiteral("Audio") },
        { QStringLiteral("webdav.videoModeHint"), QStringLiteral("Folders and videos only") },
        { QStringLiteral("webdav.audioModeHint"), QStringLiteral("Play audio from this folder") },
        { QStringLiteral("webdav.repeatOff"), QStringLiteral("Play in order") },
        { QStringLiteral("webdav.repeatOne"), QStringLiteral("Repeat current track") },
        { QStringLiteral("webdav.repeatAll"), QStringLiteral("Repeat queue") },
        { QStringLiteral("webdav.openAudioPlayer"), QStringLiteral("Open audio player") },
        { QStringLiteral("webdav.folder"), QStringLiteral("Folder") },
        { QStringLiteral("webdav.video"), QStringLiteral("Video") },
        { QStringLiteral("webdav.audio"), QStringLiteral("Audio") },
        { QStringLiteral("webdav.loadingFolder"), QStringLiteral("Loading folder...") },
        { QStringLiteral("webdav.loadingHint"), QStringLiteral("Reading remote directory") },
        { QStringLiteral("webdav.defaultDownload"), QStringLiteral("Default download folder") },
        { QStringLiteral("webdav.noDownloadFolder"), QStringLiteral("Ask every time") },
        { QStringLiteral("webdav.spaceWarningTitle"), QStringLiteral("Storage check") },
        { QStringLiteral("webdav.spaceWarning"), QStringLiteral("Download size is %1, available disk space is %2. Continue anyway?") },
        { QStringLiteral("webdav.unknownSizeWarning"), QStringLiteral("The total download size could not be confirmed. Continue anyway?") },
        { QStringLiteral("webdav.tsslRestore"), QStringLiteral("Restore TSSL") },
        { QStringLiteral("webdav.tsslExport"), QStringLiteral("Export TSSL") },
        { QStringLiteral("webdav.tsslRestored"), QStringLiteral("TSSL restored to local secure storage") },
        { QStringLiteral("webdav.tsslExported"), QStringLiteral("TSSL export completed") },
        { QStringLiteral("transfers.title"), QStringLiteral("Transfers") },
        { QStringLiteral("transfers.subtitle"), QStringLiteral("Download queue and recent activity") },
        { QStringLiteral("transfers.detailsSubtitle"), QStringLiteral("File progress for this download") },
        { QStringLiteral("transfers.empty"), QStringLiteral("No transfer tasks") },
        { QStringLiteral("transfers.emptyHint"), QStringLiteral("Downloads and uploads will appear here") },
        { QStringLiteral("transfers.emptyDetails"), QStringLiteral("This download contains no file tasks") },
        { QStringLiteral("transfers.emptyFiltered"), QStringLiteral("No files match this status") },
        { QStringLiteral("transfers.emptyFilteredHint"), QStringLiteral("This download has no files in the selected state") },
        { QStringLiteral("transfers.filterAll"), QStringLiteral("All") },
        { QStringLiteral("transfers.filterIncomplete"), QStringLiteral("Incomplete") },
        { QStringLiteral("transfers.filterCompleted"), QStringLiteral("Completed") },
        { QStringLiteral("transfers.filterFailed"), QStringLiteral("Failed") },
        { QStringLiteral("transfers.filterCanceled"), QStringLiteral("Canceled") },
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
        { QStringLiteral("transfers.pauseUpload"), QStringLiteral("Pause upload") },
        { QStringLiteral("transfers.resumeUpload"), QStringLiteral("Resume upload") },
        { QStringLiteral("transfers.retryTask"), QStringLiteral("Retry all failed or canceled files") },
        { QStringLiteral("transfers.retryFile"), QStringLiteral("Retry this file") },
        { QStringLiteral("transfers.retryUpload"), QStringLiteral("Re-upload this file") },
        { QStringLiteral("transfers.cancelTask"), QStringLiteral("Cancel task and delete local files") },
        { QStringLiteral("transfers.cancelFile"), QStringLiteral("Cancel file and delete local file") },
        { QStringLiteral("transfers.cancelUpload"), QStringLiteral("Cancel upload and keep local file") },
        { QStringLiteral("status.autoLogin"), QStringLiteral("Auto login") },
        { QStringLiteral("status.passwordRequired"), QStringLiteral("Password required") },
        { QStringLiteral("status.ready"), QStringLiteral("Ready") },
        { QStringLiteral("status.noSession"), QStringLiteral("No session") },
        { QStringLiteral("empty.noServices"), QStringLiteral("No services yet") },
        { QStringLiteral("empty.addService"), QStringLiteral("Add service") },
        { QStringLiteral("local.title"), QStringLiteral("Local Playback") },
        { QStringLiteral("local.subtitle"), QStringLiteral("Browse and play videos stored on this device") },
        { QStringLiteral("local.builtIn"), QStringLiteral("Built in") },
        { QStringLiteral("local.folderCount"), QStringLiteral("%1 folders") },
        { QStringLiteral("local.addFolder"), QStringLiteral("Add folder") },
        { QStringLiteral("local.foldersTitle"), QStringLiteral("Video folders") },
        { QStringLiteral("local.foldersSubtitle"), QStringLiteral("Only the selected folder is browsed; no metadata or posters are generated") },
        { QStringLiteral("local.noFolders"), QStringLiteral("No local folders added") },
        { QStringLiteral("local.noFoldersHint"), QStringLiteral("Add a folder that contains video files") },
        { QStringLiteral("local.folderUnavailable"), QStringLiteral("This folder is unavailable or cannot be read") },
        { QStringLiteral("local.fileUnavailable"), QStringLiteral("This video file is no longer available") },
        { QStringLiteral("local.unavailable"), QStringLiteral("Unavailable") },
        { QStringLiteral("local.available"), QStringLiteral("Available") },
        { QStringLiteral("local.remove"), QStringLiteral("Remove") },
        { QStringLiteral("local.back"), QStringLiteral("Back") },
        { QStringLiteral("local.noVideos"), QStringLiteral("No folders or videos here") },
        { QStringLiteral("local.noVideosHint"), QStringLiteral("Supported video files will appear automatically") },
        { QStringLiteral("local.loading"), QStringLiteral("Reading local folder") },
        { QStringLiteral("local.folder"), QStringLiteral("Folder") },
        { QStringLiteral("local.video"), QStringLiteral("Video") },
        { QStringLiteral("local.dropVideo"), QStringLiteral("Drop video to play") },
        { QStringLiteral("local.dropVideoHint"), QStringLiteral("Release to open this local video") },
        { QStringLiteral("local.dropUnsupported"), QStringLiteral("Drop a supported local video file") },
        { QStringLiteral("link.title"), QStringLiteral("Link Playback") },
        { QStringLiteral("link.subtitle"), QStringLiteral("Play direct HTTP/HTTPS media and HLS streams") },
        { QStringLiteral("link.protocols"), QStringLiteral("HTTP/HTTPS · HLS") },
        { QStringLiteral("link.formTitle"), QStringLiteral("Open a media link") },
        { QStringLiteral("link.formSubtitle"), QStringLiteral("Paste a direct media URL or an HLS manifest and play it with the built-in player") },
        { QStringLiteral("link.address"), QStringLiteral("Playback link") },
        { QStringLiteral("link.placeholder"), QStringLiteral("https://media.example.com/video.mp4 or index.m3u8") },
        { QStringLiteral("link.playNow"), QStringLiteral("Play now") },
        { QStringLiteral("link.supportedHint"), QStringLiteral("Direct media URLs and HLS manifests are supported. IPTV channel lists should be added through IPTV.") },
        { QStringLiteral("link.historyStorage"), QStringLiteral("Played links are saved locally so you can open them again from history.") },
        { QStringLiteral("link.historyTitle"), QStringLiteral("Playback history") },
        { QStringLiteral("link.historySubtitle"), QStringLiteral("Saved by playback date. Select a record to play it again.") },
        { QStringLiteral("link.historyEmpty"), QStringLiteral("No link playback history yet") },
        { QStringLiteral("link.playAgain"), QStringLiteral("Play again") },
        { QStringLiteral("link.deleteHistory"), QStringLiteral("Delete") },
        { QStringLiteral("link.historyLoadFailed"), QStringLiteral("Unable to load link playback history") },
        { QStringLiteral("link.historyDeleteFailed"), QStringLiteral("Unable to delete the history record") },
        { QStringLiteral("link.historyInvalid"), QStringLiteral("This saved playback link is no longer valid") },
        { QStringLiteral("link.mediaType"), QStringLiteral("Link Stream") },
        { QStringLiteral("link.errorEmpty"), QStringLiteral("Enter a playback link") },
        { QStringLiteral("link.errorTooLong"), QStringLiteral("The playback link is too long") },
        { QStringLiteral("link.errorInvalid"), QStringLiteral("Enter a valid absolute HTTP or HTTPS link") },
        { QStringLiteral("link.errorScheme"), QStringLiteral("Only HTTP and HTTPS playback links are supported") },
        { QStringLiteral("link.errorHost"), QStringLiteral("The playback link must include a host") },
        { QStringLiteral("link.errorCredentials"), QStringLiteral("Credentials embedded in playback links are not supported") },
        { QStringLiteral("globalHistory.title"), QStringLiteral("Global History") },
        { QStringLiteral("globalHistory.subtitle"), QStringLiteral("Playback history across every media source") },
        { QStringLiteral("globalHistory.cardSubtitle"), QStringLiteral("Continue watching from any connected source") },
        { QStringLiteral("globalHistory.builtIn"), QStringLiteral("All sources") },
        { QStringLiteral("m3u8s.title"), QStringLiteral("M3U8S Video Manager") },
        { QStringLiteral("m3u8s.subtitle"), QStringLiteral("Create encrypted HLS packages and manage local TSSL keys") },
        { QStringLiteral("m3u8s.cardSubtitle"), QStringLiteral("AES-256-GCM packaging and local key storage") },
        { QStringLiteral("m3u8s.builtIn"), QStringLiteral("Secure HLS") },
        { QStringLiteral("m3u8s.packageCount"), QStringLiteral("%1 TSSL packages") },
        { QStringLiteral("m3u8s.createTitle"), QStringLiteral("Create encrypted video package") },
        { QStringLiteral("m3u8s.createSubtitle"), QStringLiteral("Choose a local video. The output contains an M3U8S manifest, authenticated TS segments and a TSSL key package.") },
        { QStringLiteral("m3u8s.createAction"), QStringLiteral("Choose video and create") },
        { QStringLiteral("m3u8s.segmentDuration"), QStringLiteral("Segment duration") },
        { QStringLiteral("m3u8s.seconds"), QStringLiteral("%1 seconds") },
        { QStringLiteral("m3u8s.ffmpegReady"), QStringLiteral("FFmpeg ready") },
        { QStringLiteral("m3u8s.ffmpegMissing"), QStringLiteral("FFmpeg not found") },
        { QStringLiteral("m3u8s.phase.segmenting"), QStringLiteral("Transcoding and segmenting video") },
        { QStringLiteral("m3u8s.phase.encrypting"), QStringLiteral("Encrypting and verifying TS segments") },
        { QStringLiteral("m3u8s.phase.finalizing"), QStringLiteral("Writing manifest and TSSL package") },
        { QStringLiteral("m3u8s.phase.canceling"), QStringLiteral("Canceling and cleaning temporary files") },
        { QStringLiteral("m3u8s.processingStatus"), QStringLiteral("Packaging is in progress. The source video will not be modified.") },
        { QStringLiteral("m3u8s.completedStatus"), QStringLiteral("Package completed with %1 encrypted TS segments") },
        { QStringLiteral("m3u8s.failedStatus"), QStringLiteral("Package creation failed") },
        { QStringLiteral("m3u8s.canceledStatus"), QStringLiteral("Package creation canceled") },
        { QStringLiteral("m3u8s.savedTitle"), QStringLiteral("Saved TSSL packages") },
        { QStringLiteral("m3u8s.savedSubtitle"), QStringLiteral("Keys remain on this device and are matched to M3U8S manifests by a 4096-character identifier") },
        { QStringLiteral("m3u8s.noPackages"), QStringLiteral("No TSSL packages saved") },
        { QStringLiteral("m3u8s.noPackagesHint"), QStringLiteral("Create an encrypted video package or import a TSSL backup") },
        { QStringLiteral("m3u8s.identifier"), QStringLiteral("Identifier") },
        { QStringLiteral("m3u8s.identifierLength"), QStringLiteral("%1 characters") },
        { QStringLiteral("m3u8s.segments"), QStringLiteral("%1 TS segments") },
        { QStringLiteral("m3u8s.importTssl"), QStringLiteral("Import TSSL") },
        { QStringLiteral("m3u8s.exportTssl"), QStringLiteral("Export TSSL") },
        { QStringLiteral("m3u8s.deleteTssl"), QStringLiteral("Delete TSSL") },
        { QStringLiteral("m3u8s.deleteTitle"), QStringLiteral("Delete local TSSL?") },
        { QStringLiteral("m3u8s.deletePrompt"), QStringLiteral("The encrypted video cannot be decrypted on this device without this key package.") },
        { QStringLiteral("m3u8s.openStorage"), QStringLiteral("Open key storage") },
        { QStringLiteral("m3u8s.openOutput"), QStringLiteral("Open output folder") },
        { QStringLiteral("m3u8s.restoredStatus"), QStringLiteral("TSSL imported into local key storage") },
        { QStringLiteral("m3u8s.exportedStatus"), QStringLiteral("TSSL backup exported") },
        { QStringLiteral("m3u8s.deletedStatus"), QStringLiteral("Local TSSL package deleted") },
        { QStringLiteral("m3u8s.invalidPackage"), QStringLiteral("The selected TSSL package is no longer available") },
        { QStringLiteral("m3u8s.invalidSavedPackage"), QStringLiteral("Legacy or invalid TSSL package") },
        { QStringLiteral("m3u8s.chooseVideo"), QStringLiteral("Choose a video to package") },
        { QStringLiteral("m3u8s.chooseOutput"), QStringLiteral("Choose an output folder") },
        { QStringLiteral("m3u8s.openFolderFailed"), QStringLiteral("The folder could not be opened") },
        { QStringLiteral("globalHistory.localIndex"), QStringLiteral("Local index") },
        { QStringLiteral("globalHistory.recordCount"), QStringLiteral("%1 loaded") },
        { QStringLiteral("globalHistory.recentTitle"), QStringLiteral("Recent playback") },
        { QStringLiteral("globalHistory.recentSubtitle"), QStringLiteral("Newest activity first, with progress kept locally") },
        { QStringLiteral("globalHistory.filterAll"), QStringLiteral("All") },
        { QStringLiteral("globalHistory.sourceEmby"), QStringLiteral("Emby") },
        { QStringLiteral("globalHistory.sourceJellyfin"), QStringLiteral("Jellyfin") },
        { QStringLiteral("globalHistory.sourceWebDav"), QStringLiteral("WebDAV") },
        { QStringLiteral("globalHistory.sourceIptv"), QStringLiteral("IPTV") },
        { QStringLiteral("globalHistory.sourceLocal"), QStringLiteral("Local") },
        { QStringLiteral("globalHistory.sourceLink"), QStringLiteral("Link") },
        { QStringLiteral("globalHistory.privateIncluded"), QStringLiteral("Private records included") },
        { QStringLiteral("globalHistory.privateBadge"), QStringLiteral("PRIVATE") },
        { QStringLiteral("globalHistory.unavailable"), QStringLiteral("Unavailable") },
        { QStringLiteral("globalHistory.completed"), QStringLiteral("Completed") },
        { QStringLiteral("globalHistory.resumeAt"), QStringLiteral("Resume at %1") },
        { QStringLiteral("globalHistory.started"), QStringLiteral("Started") },
        { QStringLiteral("globalHistory.empty"), QStringLiteral("No playback history yet") },
        { QStringLiteral("globalHistory.emptyHint"), QStringLiteral("Items appear here after playback starts") },
        { QStringLiteral("globalHistory.loading"), QStringLiteral("Loading playback history") },
        { QStringLiteral("globalHistory.loadingHint"), QStringLiteral("Reading the local history index") },
        { QStringLiteral("globalHistory.loadMore"), QStringLiteral("Load more") },
        { QStringLiteral("globalHistory.loadFailed"), QStringLiteral("Playback history could not be loaded") },
        { QStringLiteral("globalHistory.deleteFailed"), QStringLiteral("This history record could not be deleted") },
        { QStringLiteral("globalHistory.invalid"), QStringLiteral("This playback history record is no longer valid") },
        { QStringLiteral("globalHistory.localUnavailable"), QStringLiteral("The local media file is no longer available") },
        { QStringLiteral("globalHistory.linkExpired"), QStringLiteral("The saved playback link is no longer valid") },
        { QStringLiteral("globalHistory.serviceUnavailable"), QStringLiteral("The source service is unavailable or has been removed") },
        { QStringLiteral("globalHistory.channelUnavailable"), QStringLiteral("The IPTV channel is no longer in this playlist") },
        { QStringLiteral("globalHistory.webDavUnavailable"), QStringLiteral("The WebDAV item is no longer available from this service") },
        { QStringLiteral("section.continueWatching"), QStringLiteral("Continue Watching") },
        { QStringLiteral("section.continueSubtitle"), QStringLiteral("Resume progress opens the media details page") },
        { QStringLiteral("section.noProgress"), QStringLiteral("Nothing in progress") },
        { QStringLiteral("section.libraries"), QStringLiteral("Libraries") },
        { QStringLiteral("section.librariesSubtitle"), QStringLiteral("Browse server media categories") },
        { QStringLiteral("search.serverPlaceholder"), QStringLiteral("Search all videos") },
        { QStringLiteral("search.action"), QStringLiteral("Search") },
        { QStringLiteral("search.clear"), QStringLiteral("Clear search") },
        { QStringLiteral("search.results"), QStringLiteral("Search results") },
        { QStringLiteral("search.resultsFor"), QStringLiteral("Results for") },
        { QStringLiteral("search.resultCount"), QStringLiteral("%1 results") },
        { QStringLiteral("search.noResults"), QStringLiteral("No matching videos found") },
        { QStringLiteral("search.noResultsHint"), QStringLiteral("Try a shorter title or a different keyword") },
        { QStringLiteral("search.loading"), QStringLiteral("Searching server") },
        { QStringLiteral("search.loadingHint"), QStringLiteral("Reading matching videos from the current server") },
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
        { QStringLiteral("player.loadSubtitle"), QStringLiteral("Load external subtitle") },
        { QStringLiteral("player.selectSubtitleFile"), QStringLiteral("Select subtitle file") },
        { QStringLiteral("player.subtitleFiles"), QStringLiteral("Subtitle files") },
        { QStringLiteral("player.allFiles"), QStringLiteral("All files") },
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
        { QStringLiteral("settings.pageTransitions"), QStringLiteral("Page transition animations") },
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
        { QStringLiteral("history.download"), QStringLiteral("Download") },
        { QStringLiteral("history.upload"), QStringLiteral("Upload") },
        { QStringLiteral("history.totalDownload"), QStringLiteral("Total download") },
        { QStringLiteral("history.totalUpload"), QStringLiteral("Total upload") },
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
        { QStringLiteral("schedule.subtitle"), QStringLiteral("Create manual or recurring silent background playback strategies for Emby") },
        { QStringLiteral("schedule.add"), QStringLiteral("New Strategy") },
        { QStringLiteral("schedule.edit"), QStringLiteral("Edit Strategy") },
        { QStringLiteral("schedule.source"), QStringLiteral("Emby source") },
        { QStringLiteral("schedule.duration"), QStringLiteral("Playback duration") },
        { QStringLiteral("schedule.minutes"), QStringLiteral("minutes") },
        { QStringLiteral("schedule.type"), QStringLiteral("Run strategy") },
        { QStringLiteral("schedule.typeManual"), QStringLiteral("Manual only") },
        { QStringLiteral("schedule.typeDaily"), QStringLiteral("Every day") },
        { QStringLiteral("schedule.typeWeekly"), QStringLiteral("Every week") },
        { QStringLiteral("schedule.typeMonthly"), QStringLiteral("Every month") },
        { QStringLiteral("schedule.typeCustomMonthly"), QStringLiteral("Custom monthly dates") },
        { QStringLiteral("schedule.startTime"), QStringLiteral("Start time") },
        { QStringLiteral("schedule.weekday"), QStringLiteral("Day of week") },
        { QStringLiteral("schedule.monthDay"), QStringLiteral("Day of month") },
        { QStringLiteral("schedule.customDays"), QStringLiteral("Monthly dates") },
        { QStringLiteral("schedule.enabled"), QStringLiteral("Automatic scheduling") },
        { QStringLiteral("schedule.enabledBadge"), QStringLiteral("Enabled") },
        { QStringLiteral("schedule.disabledBadge"), QStringLiteral("Paused") },
        { QStringLiteral("schedule.save"), QStringLiteral("Save Strategy") },
        { QStringLiteral("schedule.saveAndRun"), QStringLiteral("Save & Start Now") },
        { QStringLiteral("schedule.runNow"), QStringLiteral("Start Now") },
        { QStringLiteral("schedule.manualHint"), QStringLiteral("Save the strategy for manual starts, or save and start it immediately.") },
        { QStringLiteral("schedule.scheduledHint"), QStringLiteral("Automatic runs use the computer's local time. Foreground playback always takes priority.") },
        { QStringLiteral("schedule.empty"), QStringLiteral("No saved keep-alive strategies") },
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
        { QStringLiteral("schedule.errorCustomDays"), QStringLiteral("Select at least one monthly date") },
        { QStringLiteral("schedule.missedTitle"), QStringLiteral("Missed keep-alive tasks") },
        { QStringLiteral("schedule.missedIgnore"), QStringLiteral("Skip These Runs") },
        { QStringLiteral("schedule.missedRun"), QStringLiteral("Run Now") },
        { QStringLiteral("schedule.weekday1"), QStringLiteral("Monday") },
        { QStringLiteral("schedule.weekday2"), QStringLiteral("Tuesday") },
        { QStringLiteral("schedule.weekday3"), QStringLiteral("Wednesday") },
        { QStringLiteral("schedule.weekday4"), QStringLiteral("Thursday") },
        { QStringLiteral("schedule.weekday5"), QStringLiteral("Friday") },
        { QStringLiteral("schedule.weekday6"), QStringLiteral("Saturday") },
        { QStringLiteral("schedule.weekday7"), QStringLiteral("Sunday") },
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
        { QStringLiteral("action.previous"), QStringLiteral("上一首") },
        { QStringLiteral("action.next"), QStringLiteral("下一首") },
        { QStringLiteral("dialog.certificateTitle"), QStringLiteral("证书确认") },
        { QStringLiteral("dialog.certificatePrefix"), QStringLiteral("服务器 ") },
        { QStringLiteral("dialog.certificateSuffix"), QStringLiteral(" 的证书无法验证。是否继续本次请求？") },
        { QStringLiteral("dialog.passwordTitle"), QStringLiteral("需要密码") },
        { QStringLiteral("dialog.serviceTitle"), QStringLiteral("服务") },
        { QStringLiteral("dialog.deleteTitle"), QStringLiteral("删除服务") },
        { QStringLiteral("dialog.deletePrompt"), QStringLiteral("移除此服务卡片？") },
        { QStringLiteral("dialog.deleteLocalData"), QStringLiteral("同时删除本地 Token 和缓存数据") },
        { QStringLiteral("dialog.exitPlaybackTitle"), QStringLiteral("退出播放？") },
        { QStringLiteral("dialog.exitPlaybackPrompt"), QStringLiteral("播放将停止，并返回上一页。") },
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
        { QStringLiteral("local.title"), QStringLiteral("本地播放") },
        { QStringLiteral("local.subtitle"), QStringLiteral("浏览并播放此设备中保存的视频") },
        { QStringLiteral("local.builtIn"), QStringLiteral("内置功能") },
        { QStringLiteral("local.folderCount"), QStringLiteral("%1 个目录") },
        { QStringLiteral("local.addFolder"), QStringLiteral("添加目录") },
        { QStringLiteral("local.foldersTitle"), QStringLiteral("视频目录") },
        { QStringLiteral("local.foldersSubtitle"), QStringLiteral("仅浏览所选目录，不生成元数据或海报") },
        { QStringLiteral("local.noFolders"), QStringLiteral("尚未添加本地目录") },
        { QStringLiteral("local.noFoldersHint"), QStringLiteral("添加一个包含视频文件的目录") },
        { QStringLiteral("local.folderUnavailable"), QStringLiteral("该目录不可用或无法读取") },
        { QStringLiteral("local.fileUnavailable"), QStringLiteral("该视频文件已不可用") },
        { QStringLiteral("local.unavailable"), QStringLiteral("不可用") },
        { QStringLiteral("local.available"), QStringLiteral("可用") },
        { QStringLiteral("local.remove"), QStringLiteral("移除") },
        { QStringLiteral("local.back"), QStringLiteral("返回") },
        { QStringLiteral("local.noVideos"), QStringLiteral("此处没有文件夹或视频") },
        { QStringLiteral("local.noVideosHint"), QStringLiteral("支持的视频文件会自动显示") },
        { QStringLiteral("local.loading"), QStringLiteral("正在读取本地目录") },
        { QStringLiteral("local.folder"), QStringLiteral("文件夹") },
        { QStringLiteral("local.video"), QStringLiteral("视频") },
        { QStringLiteral("local.dropVideo"), QStringLiteral("拖放视频以播放") },
        { QStringLiteral("local.dropVideoHint"), QStringLiteral("松开即可打开此本地视频") },
        { QStringLiteral("local.dropUnsupported"), QStringLiteral("请拖放受支持的本地视频文件") },
        { QStringLiteral("link.title"), QStringLiteral("链接播放") },
        { QStringLiteral("link.subtitle"), QStringLiteral("播放 HTTP/HTTPS 直接媒体和 HLS 视频流") },
        { QStringLiteral("link.protocols"), QStringLiteral("HTTP/HTTPS · HLS") },
        { QStringLiteral("link.formTitle"), QStringLiteral("打开媒体链接") },
        { QStringLiteral("link.formSubtitle"), QStringLiteral("粘贴直接媒体地址或 HLS 清单，使用内置播放器播放") },
        { QStringLiteral("link.address"), QStringLiteral("播放链接") },
        { QStringLiteral("link.placeholder"), QStringLiteral("https://media.example.com/video.mp4 或 index.m3u8") },
        { QStringLiteral("link.playNow"), QStringLiteral("立即播放") },
        { QStringLiteral("link.supportedHint"), QStringLiteral("支持直接媒体地址和 HLS 清单；IPTV 频道列表请通过 IPTV 添加。") },
        { QStringLiteral("link.historyStorage"), QStringLiteral("播放过的链接会保存在本机，可从历史记录中再次打开。") },
        { QStringLiteral("link.historyTitle"), QStringLiteral("播放历史") },
        { QStringLiteral("link.historySubtitle"), QStringLiteral("按照播放日期保存，点击记录即可再次播放。") },
        { QStringLiteral("link.historyEmpty"), QStringLiteral("暂无链接播放历史") },
        { QStringLiteral("link.playAgain"), QStringLiteral("再次播放") },
        { QStringLiteral("link.deleteHistory"), QStringLiteral("删除") },
        { QStringLiteral("link.historyLoadFailed"), QStringLiteral("无法加载链接播放历史") },
        { QStringLiteral("link.historyDeleteFailed"), QStringLiteral("无法删除这条历史记录") },
        { QStringLiteral("link.historyInvalid"), QStringLiteral("这条历史记录中的播放链接已失效") },
        { QStringLiteral("link.mediaType"), QStringLiteral("链接视频流") },
        { QStringLiteral("link.errorEmpty"), QStringLiteral("请输入播放链接") },
        { QStringLiteral("link.errorTooLong"), QStringLiteral("播放链接过长") },
        { QStringLiteral("link.errorInvalid"), QStringLiteral("请输入有效的 HTTP 或 HTTPS 绝对链接") },
        { QStringLiteral("link.errorScheme"), QStringLiteral("仅支持 HTTP 和 HTTPS 播放链接") },
        { QStringLiteral("link.errorHost"), QStringLiteral("播放链接必须包含主机地址") },
        { QStringLiteral("link.errorCredentials"), QStringLiteral("暂不支持在播放链接中嵌入账号或密码") },
        { QStringLiteral("globalHistory.title"), QStringLiteral("全局历史") },
        { QStringLiteral("globalHistory.subtitle"), QStringLiteral("汇总各个媒体来源的播放历史") },
        { QStringLiteral("globalHistory.cardSubtitle"), QStringLiteral("集中查看并继续播放各类来源中的内容") },
        { QStringLiteral("globalHistory.builtIn"), QStringLiteral("全部来源") },
        { QStringLiteral("m3u8s.title"), QStringLiteral("M3U8S 视频管理") },
        { QStringLiteral("m3u8s.subtitle"), QStringLiteral("创建加密 HLS 视频包并管理本机 TSSL 密钥") },
        { QStringLiteral("m3u8s.cardSubtitle"), QStringLiteral("AES-256-GCM 打包与本机密钥存储") },
        { QStringLiteral("m3u8s.builtIn"), QStringLiteral("安全 HLS") },
        { QStringLiteral("m3u8s.packageCount"), QStringLiteral("%1 个 TSSL 密钥包") },
        { QStringLiteral("m3u8s.createTitle"), QStringLiteral("创建加密视频包") },
        { QStringLiteral("m3u8s.createSubtitle"), QStringLiteral("选择本地视频，输出 M3U8S 清单、经过认证的 TS 分片和 TSSL 密钥包。") },
        { QStringLiteral("m3u8s.createAction"), QStringLiteral("选择视频并创建") },
        { QStringLiteral("m3u8s.segmentDuration"), QStringLiteral("分片时长") },
        { QStringLiteral("m3u8s.seconds"), QStringLiteral("%1 秒") },
        { QStringLiteral("m3u8s.ffmpegReady"), QStringLiteral("FFmpeg 已就绪") },
        { QStringLiteral("m3u8s.ffmpegMissing"), QStringLiteral("未找到 FFmpeg") },
        { QStringLiteral("m3u8s.phase.segmenting"), QStringLiteral("正在转码并切分视频") },
        { QStringLiteral("m3u8s.phase.encrypting"), QStringLiteral("正在加密并验证 TS 分片") },
        { QStringLiteral("m3u8s.phase.finalizing"), QStringLiteral("正在写入清单和 TSSL 密钥包") },
        { QStringLiteral("m3u8s.phase.canceling"), QStringLiteral("正在取消并清理临时文件") },
        { QStringLiteral("m3u8s.processingStatus"), QStringLiteral("正在打包，原始视频不会被修改。") },
        { QStringLiteral("m3u8s.completedStatus"), QStringLiteral("打包完成，共生成 %1 个加密 TS 分片") },
        { QStringLiteral("m3u8s.failedStatus"), QStringLiteral("视频包创建失败") },
        { QStringLiteral("m3u8s.canceledStatus"), QStringLiteral("已取消视频包创建") },
        { QStringLiteral("m3u8s.savedTitle"), QStringLiteral("已保存的 TSSL 密钥包") },
        { QStringLiteral("m3u8s.savedSubtitle"), QStringLiteral("密钥仅保存在本机，并通过 4096 字符识别码与 M3U8S 清单匹配") },
        { QStringLiteral("m3u8s.noPackages"), QStringLiteral("尚未保存 TSSL 密钥包") },
        { QStringLiteral("m3u8s.noPackagesHint"), QStringLiteral("创建加密视频包或导入 TSSL 备份") },
        { QStringLiteral("m3u8s.identifier"), QStringLiteral("识别码") },
        { QStringLiteral("m3u8s.identifierLength"), QStringLiteral("%1 个字符") },
        { QStringLiteral("m3u8s.segments"), QStringLiteral("%1 个 TS 分片") },
        { QStringLiteral("m3u8s.importTssl"), QStringLiteral("导入 TSSL") },
        { QStringLiteral("m3u8s.exportTssl"), QStringLiteral("导出 TSSL") },
        { QStringLiteral("m3u8s.deleteTssl"), QStringLiteral("删除 TSSL") },
        { QStringLiteral("m3u8s.deleteTitle"), QStringLiteral("删除本机 TSSL？") },
        { QStringLiteral("m3u8s.deletePrompt"), QStringLiteral("删除后，本机将无法解密与该密钥包对应的视频。") },
        { QStringLiteral("m3u8s.openStorage"), QStringLiteral("打开密钥目录") },
        { QStringLiteral("m3u8s.openOutput"), QStringLiteral("打开输出目录") },
        { QStringLiteral("m3u8s.restoredStatus"), QStringLiteral("TSSL 已导入本机密钥存储") },
        { QStringLiteral("m3u8s.exportedStatus"), QStringLiteral("TSSL 备份已导出") },
        { QStringLiteral("m3u8s.deletedStatus"), QStringLiteral("本机 TSSL 密钥包已删除") },
        { QStringLiteral("m3u8s.invalidPackage"), QStringLiteral("所选 TSSL 密钥包已不可用") },
        { QStringLiteral("m3u8s.invalidSavedPackage"), QStringLiteral("旧版或无效的 TSSL 密钥包") },
        { QStringLiteral("m3u8s.chooseVideo"), QStringLiteral("选择要打包的视频") },
        { QStringLiteral("m3u8s.chooseOutput"), QStringLiteral("选择输出目录") },
        { QStringLiteral("m3u8s.openFolderFailed"), QStringLiteral("无法打开该目录") },
        { QStringLiteral("globalHistory.localIndex"), QStringLiteral("本地索引") },
        { QStringLiteral("globalHistory.recordCount"), QStringLiteral("已加载 %1 条") },
        { QStringLiteral("globalHistory.recentTitle"), QStringLiteral("最近播放") },
        { QStringLiteral("globalHistory.recentSubtitle"), QStringLiteral("按时间倒序展示，本地保存播放进度") },
        { QStringLiteral("globalHistory.filterAll"), QStringLiteral("全部") },
        { QStringLiteral("globalHistory.sourceEmby"), QStringLiteral("Emby") },
        { QStringLiteral("globalHistory.sourceJellyfin"), QStringLiteral("Jellyfin") },
        { QStringLiteral("globalHistory.sourceWebDav"), QStringLiteral("WebDAV") },
        { QStringLiteral("globalHistory.sourceIptv"), QStringLiteral("IPTV") },
        { QStringLiteral("globalHistory.sourceLocal"), QStringLiteral("本地") },
        { QStringLiteral("globalHistory.sourceLink"), QStringLiteral("链接") },
        { QStringLiteral("globalHistory.privateIncluded"), QStringLiteral("包含隐私记录") },
        { QStringLiteral("globalHistory.privateBadge"), QStringLiteral("隐私") },
        { QStringLiteral("globalHistory.unavailable"), QStringLiteral("不可用") },
        { QStringLiteral("globalHistory.completed"), QStringLiteral("已看完") },
        { QStringLiteral("globalHistory.resumeAt"), QStringLiteral("从 %1 继续") },
        { QStringLiteral("globalHistory.started"), QStringLiteral("已开始") },
        { QStringLiteral("globalHistory.empty"), QStringLiteral("暂无播放历史") },
        { QStringLiteral("globalHistory.emptyHint"), QStringLiteral("内容开始播放后会记录在这里") },
        { QStringLiteral("globalHistory.loading"), QStringLiteral("正在加载播放历史") },
        { QStringLiteral("globalHistory.loadingHint"), QStringLiteral("正在读取本地历史索引") },
        { QStringLiteral("globalHistory.loadMore"), QStringLiteral("加载更多") },
        { QStringLiteral("globalHistory.loadFailed"), QStringLiteral("无法加载播放历史") },
        { QStringLiteral("globalHistory.deleteFailed"), QStringLiteral("无法删除这条历史记录") },
        { QStringLiteral("globalHistory.invalid"), QStringLiteral("这条播放历史已失效") },
        { QStringLiteral("globalHistory.localUnavailable"), QStringLiteral("该本地媒体文件已不可用") },
        { QStringLiteral("globalHistory.linkExpired"), QStringLiteral("保存的播放链接已失效") },
        { QStringLiteral("globalHistory.serviceUnavailable"), QStringLiteral("来源服务不可用或已被移除") },
        { QStringLiteral("globalHistory.channelUnavailable"), QStringLiteral("播放列表中已找不到该 IPTV 频道") },
        { QStringLiteral("globalHistory.webDavUnavailable"), QStringLiteral("该 WebDAV 内容已无法从当前服务访问") },
        { QStringLiteral("section.continueWatching"), QStringLiteral("继续观看") },
        { QStringLiteral("section.continueSubtitle"), QStringLiteral("点击后进入媒体详情页") },
        { QStringLiteral("section.noProgress"), QStringLiteral("暂无继续观看内容") },
        { QStringLiteral("section.libraries"), QStringLiteral("媒体库") },
        { QStringLiteral("section.librariesSubtitle"), QStringLiteral("浏览服务器媒体分类") },
        { QStringLiteral("search.serverPlaceholder"), QStringLiteral("搜索所有影片") },
        { QStringLiteral("search.action"), QStringLiteral("搜索") },
        { QStringLiteral("search.clear"), QStringLiteral("清除搜索") },
        { QStringLiteral("search.results"), QStringLiteral("搜索结果") },
        { QStringLiteral("search.resultsFor"), QStringLiteral("搜索内容") },
        { QStringLiteral("search.resultCount"), QStringLiteral("%1 个结果") },
        { QStringLiteral("search.noResults"), QStringLiteral("没有找到匹配的影片") },
        { QStringLiteral("search.noResultsHint"), QStringLiteral("请尝试更短的片名或其他关键词") },
        { QStringLiteral("search.loading"), QStringLiteral("正在搜索服务器") },
        { QStringLiteral("search.loadingHint"), QStringLiteral("正在从当前服务器读取匹配影片") },
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
        { QStringLiteral("player.loadSubtitle"), QStringLiteral("加载外挂字幕") },
        { QStringLiteral("player.selectSubtitleFile"), QStringLiteral("选择字幕文件") },
        { QStringLiteral("player.subtitleFiles"), QStringLiteral("字幕文件") },
        { QStringLiteral("player.allFiles"), QStringLiteral("所有文件") },
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
        { QStringLiteral("settings.pageTransitions"), QStringLiteral("页面切换动画") },
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
        { QStringLiteral("webdav.videoEmpty"), QStringLiteral("当前文件夹中没有子文件夹或视频") },
        { QStringLiteral("webdav.audioEmpty"), QStringLiteral("当前文件夹中没有音频文件") },
        { QStringLiteral("webdav.displayMode"), QStringLiteral("显示模式") },
        { QStringLiteral("webdav.modeDefault"), QStringLiteral("默认") },
        { QStringLiteral("webdav.modeVideo"), QStringLiteral("视频") },
        { QStringLiteral("webdav.modeAudio"), QStringLiteral("音频") },
        { QStringLiteral("webdav.videoModeHint"), QStringLiteral("仅显示子文件夹和视频") },
        { QStringLiteral("webdav.audioModeHint"), QStringLiteral("播放当前文件夹中的音频") },
        { QStringLiteral("webdav.repeatOff"), QStringLiteral("顺序播放") },
        { QStringLiteral("webdav.repeatOne"), QStringLiteral("单曲循环") },
        { QStringLiteral("webdav.repeatAll"), QStringLiteral("列表循环") },
        { QStringLiteral("webdav.openAudioPlayer"), QStringLiteral("打开音频播放器") },
        { QStringLiteral("webdav.folder"), QStringLiteral("文件夹") },
        { QStringLiteral("webdav.video"), QStringLiteral("视频") },
        { QStringLiteral("webdav.audio"), QStringLiteral("音频") },
        { QStringLiteral("webdav.loadingFolder"), QStringLiteral("正在加载文件夹...") },
        { QStringLiteral("webdav.loadingHint"), QStringLiteral("正在读取远程目录") },
        { QStringLiteral("webdav.defaultDownload"), QStringLiteral("默认下载文件夹") },
        { QStringLiteral("webdav.noDownloadFolder"), QStringLiteral("每次询问") },
        { QStringLiteral("webdav.spaceWarningTitle"), QStringLiteral("存储空间检查") },
        { QStringLiteral("webdav.spaceWarning"), QStringLiteral("下载大小为 %1，可用磁盘空间为 %2。仍要继续吗？") },
        { QStringLiteral("webdav.unknownSizeWarning"), QStringLiteral("无法确认下载总大小。仍要继续吗？") },
        { QStringLiteral("webdav.tsslRestore"), QStringLiteral("恢复 TSSL") },
        { QStringLiteral("webdav.tsslExport"), QStringLiteral("导出 TSSL") },
        { QStringLiteral("webdav.tsslRestored"), QStringLiteral("TSSL 已恢复到本机存储") },
        { QStringLiteral("webdav.tsslExported"), QStringLiteral("TSSL 导出完成") },
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
        { QStringLiteral("transfers.emptyFiltered"), QStringLiteral("当前状态下没有文件") },
        { QStringLiteral("transfers.emptyFilteredHint"), QStringLiteral("本次下载没有处于所选状态的文件") },
        { QStringLiteral("transfers.filterAll"), QStringLiteral("全部") },
        { QStringLiteral("transfers.filterIncomplete"), QStringLiteral("未完成") },
        { QStringLiteral("transfers.filterCompleted"), QStringLiteral("已完成") },
        { QStringLiteral("transfers.filterFailed"), QStringLiteral("失败") },
        { QStringLiteral("transfers.filterCanceled"), QStringLiteral("已取消") },
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
        { QStringLiteral("transfers.pauseUpload"), QStringLiteral("暂停上传") },
        { QStringLiteral("transfers.resumeUpload"), QStringLiteral("继续上传") },
        { QStringLiteral("transfers.retryTask"), QStringLiteral("重试所有失败或已取消文件") },
        { QStringLiteral("transfers.retryFile"), QStringLiteral("重试此文件") },
        { QStringLiteral("transfers.retryUpload"), QStringLiteral("重新上传此文件") },
        { QStringLiteral("transfers.cancelTask"), QStringLiteral("取消任务并删除本地文件") },
        { QStringLiteral("transfers.cancelFile"), QStringLiteral("取消此文件并删除本地文件") },
        { QStringLiteral("transfers.cancelUpload"), QStringLiteral("取消上传并保留本地文件") },
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
        { QStringLiteral("history.download"), QStringLiteral("下行") },
        { QStringLiteral("history.upload"), QStringLiteral("上行") },
        { QStringLiteral("history.totalDownload"), QStringLiteral("总下行") },
        { QStringLiteral("history.totalUpload"), QStringLiteral("总上行") },
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
        { QStringLiteral("schedule.subtitle"), QStringLiteral("为 Emby 创建手动或周期运行的无声后台保号策略") },
        { QStringLiteral("schedule.add"), QStringLiteral("新建策略") },
        { QStringLiteral("schedule.edit"), QStringLiteral("编辑策略") },
        { QStringLiteral("schedule.source"), QStringLiteral("Emby 播放源") },
        { QStringLiteral("schedule.duration"), QStringLiteral("播放时长") },
        { QStringLiteral("schedule.minutes"), QStringLiteral("分钟") },
        { QStringLiteral("schedule.type"), QStringLiteral("运行策略") },
        { QStringLiteral("schedule.typeManual"), QStringLiteral("仅手动运行") },
        { QStringLiteral("schedule.typeDaily"), QStringLiteral("每天") },
        { QStringLiteral("schedule.typeWeekly"), QStringLiteral("每周") },
        { QStringLiteral("schedule.typeMonthly"), QStringLiteral("每月") },
        { QStringLiteral("schedule.typeCustomMonthly"), QStringLiteral("自定义每月日期") },
        { QStringLiteral("schedule.startTime"), QStringLiteral("开始时间") },
        { QStringLiteral("schedule.weekday"), QStringLiteral("星期") },
        { QStringLiteral("schedule.monthDay"), QStringLiteral("每月日期") },
        { QStringLiteral("schedule.customDays"), QStringLiteral("选择日期") },
        { QStringLiteral("schedule.enabled"), QStringLiteral("自动运行策略") },
        { QStringLiteral("schedule.enabledBadge"), QStringLiteral("已启用") },
        { QStringLiteral("schedule.disabledBadge"), QStringLiteral("已暂停") },
        { QStringLiteral("schedule.save"), QStringLiteral("保存策略") },
        { QStringLiteral("schedule.saveAndRun"), QStringLiteral("保存并立即开始") },
        { QStringLiteral("schedule.runNow"), QStringLiteral("立即开始") },
        { QStringLiteral("schedule.manualHint"), QStringLiteral("可以仅保存供以后手动启动，也可以保存后立即开始。") },
        { QStringLiteral("schedule.scheduledHint"), QStringLiteral("自动任务按电脑本地时间运行；正常前台播放始终优先。") },
        { QStringLiteral("schedule.empty"), QStringLiteral("暂无保号策略") },
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
        { QStringLiteral("schedule.errorCustomDays"), QStringLiteral("请至少选择一个每月运行日期") },
        { QStringLiteral("schedule.missedTitle"), QStringLiteral("发现未执行的保号任务") },
        { QStringLiteral("schedule.missedIgnore"), QStringLiteral("本次忽略") },
        { QStringLiteral("schedule.missedRun"), QStringLiteral("立即补跑") },
        { QStringLiteral("schedule.weekday1"), QStringLiteral("星期一") },
        { QStringLiteral("schedule.weekday2"), QStringLiteral("星期二") },
        { QStringLiteral("schedule.weekday3"), QStringLiteral("星期三") },
        { QStringLiteral("schedule.weekday4"), QStringLiteral("星期四") },
        { QStringLiteral("schedule.weekday5"), QStringLiteral("星期五") },
        { QStringLiteral("schedule.weekday6"), QStringLiteral("星期六") },
        { QStringLiteral("schedule.weekday7"), QStringLiteral("星期日") },
    };
    return texts;
}
}

AppViewModel::AppViewModel(QObject* parent)
    : QObject(parent)
    , m_embyClient(m_embyNetworkClient, this)
    , m_jellyfinClient(m_jellyfinNetworkClient, this)
    , m_webDavDownloadPlanner(m_webDavClient)
    , m_encryptedHlsPlaybackProxy(m_tsslStore, this)
    , m_m3u8sPackager(m_tsslStore, this)
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
    connect(&m_m3u8sPackager, &EncryptedHlsPackager::runningChanged, this, &AppViewModel::m3u8sPackagingChanged);
    connect(&m_m3u8sPackager, &EncryptedHlsPackager::progressChanged, this, &AppViewModel::m3u8sPackagingChanged);
    connect(&m_m3u8sPackager, &EncryptedHlsPackager::phaseChanged, this, &AppViewModel::m3u8sPackagingChanged);
    connect(&m_m3u8sPackager,
            &EncryptedHlsPackager::completed,
            this,
            [this](const EncryptedHlsPackageResult& result) {
                m_m3u8sLastOutputDirectory = result.outputDirectory;
                m_m3u8sStatus = trText(QStringLiteral("m3u8s.completedStatus")).arg(result.segmentCount);
                refreshTsslPackages();
                emit m3u8sStatusChanged();
            });
    connect(&m_m3u8sPackager, &EncryptedHlsPackager::failed, this, [this](const QString& error) {
        m_m3u8sStatus = trText(QStringLiteral("m3u8s.failedStatus"));
        emit m3u8sStatusChanged();
        setError(error);
    });
    connect(&m_m3u8sPackager, &EncryptedHlsPackager::canceled, this, [this]() {
        m_m3u8sStatus = trText(QStringLiteral("m3u8s.canceledStatus"));
        emit m3u8sStatusChanged();
    });
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
    connect(&m_scheduledPlaybackManager,
            &ScheduledPlaybackManager::scheduleStateChanged,
            this,
            &AppViewModel::refreshScheduledPlaybackTasks);
    connect(&m_scheduledPlaybackManager,
            &ScheduledPlaybackManager::missedTasksChanged,
            this,
            &AppViewModel::missedScheduledPlaybackTasksChanged);
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

LocalMediaRootListModel* AppViewModel::localMediaRoots()
{
    return &m_localMediaRoots;
}

LocalMediaItemListModel* AppViewModel::localMediaItems()
{
    return &m_localMediaItems;
}

QString AppViewModel::localMediaCurrentPath() const
{
    return m_localMediaCurrentPath;
}

QString AppViewModel::localMediaRootName() const
{
    return m_currentLocalMediaRoot ? m_currentLocalMediaRoot->name : QString {};
}

bool AppViewModel::localMediaDirectoryOpen() const
{
    return m_currentLocalMediaRoot.has_value() && !m_localMediaCurrentPath.isEmpty();
}

bool AppViewModel::localMediaLoading() const
{
    return m_localMediaLoading;
}

QString AppViewModel::linkPlaybackAddress() const
{
    return m_linkPlaybackAddress;
}

void AppViewModel::setLinkPlaybackAddress(const QString& value)
{
    if (m_linkPlaybackAddress == value) {
        return;
    }
    m_linkPlaybackAddress = value;
    emit linkPlaybackAddressChanged();
}

LinkPlaybackHistoryListModel* AppViewModel::linkPlaybackHistory()
{
    return &m_linkPlaybackHistory;
}

WebDavItemListModel* AppViewModel::webDavItems()
{
    return &m_webDavItems;
}

QString AppViewModel::webDavCurrentPath() const
{
    return m_webDavCurrentUrl.path(QUrl::FullyDecoded);
}

QString AppViewModel::webDavDisplayMode() const
{
    return m_webDavDisplayMode;
}

QString AppViewModel::webDavTsslStatus() const
{
    return m_webDavTsslStatus;
}

TsslPackageListModel* AppViewModel::tsslPackages()
{
    return &m_tsslPackages;
}

bool AppViewModel::m3u8sPackaging() const
{
    return m_m3u8sPackager.isRunning();
}

double AppViewModel::m3u8sPackagingProgress() const
{
    return m_m3u8sPackager.progress();
}

QString AppViewModel::m3u8sPackagingPhase() const
{
    return m_m3u8sPackager.phase();
}

QString AppViewModel::m3u8sStatus() const
{
    return m_m3u8sStatus;
}

QString AppViewModel::m3u8sLastOutputDirectory() const
{
    return m_m3u8sLastOutputDirectory;
}

bool AppViewModel::m3u8sFfmpegAvailable() const
{
    return !m_m3u8sPackager.ffmpegExecutable().isEmpty();
}

int AppViewModel::m3u8sSegmentDuration() const
{
    return m_m3u8sSegmentDuration;
}

void AppViewModel::setM3u8sSegmentDuration(int value)
{
    const auto normalized = std::clamp(value, 2, 30);
    if (m_m3u8sSegmentDuration == normalized) {
        return;
    }
    m_m3u8sSegmentDuration = normalized;
    emit m3u8sSegmentDurationChanged();
}

bool AppViewModel::webDavAudioPlaybackActive() const
{
    return m_webDavAudioPlaybackActive;
}

int AppViewModel::webDavAudioCurrentIndex() const
{
    return m_webDavAudioCurrentIndex;
}

int AppViewModel::webDavAudioQueueCount() const
{
    return static_cast<int>(m_webDavAudioQueue.size());
}

QString AppViewModel::webDavAudioCurrentName() const
{
    if (m_webDavAudioCurrentIndex < 0 ||
        m_webDavAudioCurrentIndex >= static_cast<int>(m_webDavAudioQueue.size())) {
        return {};
    }
    return m_webDavAudioQueue[static_cast<size_t>(m_webDavAudioCurrentIndex)].name;
}

QString AppViewModel::webDavAudioRepeatMode() const
{
    return m_webDavAudioRepeatMode;
}

void AppViewModel::setWebDavAudioRepeatMode(const QString& value)
{
    const auto normalizedMode = value.compare(QStringLiteral("one"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("one")
        : value.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("all")
            : QStringLiteral("off");
    if (m_webDavAudioRepeatMode == normalizedMode) {
        return;
    }
    m_webDavAudioRepeatMode = normalizedMode;
    emit webDavAudioRepeatModeChanged();
    AppLogger::info(QStringLiteral("webdav"),
                    QStringLiteral("Audio repeat mode changed to %1").arg(m_webDavAudioRepeatMode));
}

void AppViewModel::setWebDavDisplayMode(const QString& value)
{
    const auto normalizedMode = value.compare(QStringLiteral("video"), Qt::CaseInsensitive) == 0
        ? QStringLiteral("video")
        : value.compare(QStringLiteral("audio"), Qt::CaseInsensitive) == 0
            ? QStringLiteral("audio")
            : QStringLiteral("default");
    if (m_webDavDisplayMode == normalizedMode) {
        if (normalizedMode == QStringLiteral("audio") &&
            !m_webDavAudioPlaybackActive && m_currentView == QStringLiteral("webdav") &&
            !m_webDavAudioQueue.empty()) {
            startWebDavAudioPlayback();
        }
        return;
    }

    if (m_webDavAudioPlaybackActive && normalizedMode != QStringLiteral("audio")) {
        clearWebDavAudioPlayback();
    }
    m_webDavDisplayMode = normalizedMode;
    m_webDavItems.setDisplayMode(m_webDavDisplayMode);
    rebuildWebDavAudioQueue(m_webDavDirectoryItems);
    emit webDavDisplayModeChanged();
    if (m_webDavDisplayMode == QStringLiteral("audio") &&
        m_currentWebDavCard && m_currentView == QStringLiteral("webdav") &&
        !m_webDavAudioQueue.empty()) {
        startWebDavAudioPlayback();
    }
    AppLogger::info(QStringLiteral("webdav"),
                    QStringLiteral("Display mode changed to %1").arg(m_webDavDisplayMode));
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

QString AppViewModel::transferDetailFilter() const
{
    return m_transferDetailFilter;
}

void AppViewModel::setTransferDetailFilter(const QString& value)
{
    m_transferManager.detailTasks()->setStatusFilter(value);
    const auto normalized = m_transferManager.detailTasks()->statusFilter();
    if (m_transferDetailFilter == normalized) {
        return;
    }
    m_transferDetailFilter = normalized;
    emit transferDetailFilterChanged();
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
    emit missedScheduledPlaybackTasksChanged();
}

bool AppViewModel::pageTransitionsEnabled() const
{
    return m_repository.pageTransitionsEnabled();
}

void AppViewModel::setPageTransitionsEnabled(bool value)
{
    if (pageTransitionsEnabled() == value) {
        return;
    }
    m_repository.setPageTransitionsEnabled(value);
    emit pageTransitionsEnabledChanged();
}

int AppViewModel::translationRevision() const
{
    return m_translationRevision;
}

bool AppViewModel::loading() const
{
    return m_loading;
}

bool AppViewModel::episodeSwitching() const
{
    return m_episodeSwitching;
}

bool AppViewModel::homeLoading() const
{
    return m_homeLoadingRequests > 0;
}

bool AppViewModel::libraryItemsLoading() const
{
    return m_libraryItemsLoading;
}

QString AppViewModel::serverSearchText() const
{
    return m_serverSearchText;
}

void AppViewModel::setServerSearchText(const QString& value)
{
    if (m_serverSearchText == value) {
        return;
    }
    m_serverSearchText = value;
    emit serverSearchChanged();
}

QString AppViewModel::activeServerSearchTerm() const
{
    return m_activeServerSearchTerm;
}

bool AppViewModel::serverSearchAvailable() const
{
    return m_session
        && (m_session->server.serviceType == ServiceType::Emby
            || m_session->server.serviceType == ServiceType::Jellyfin);
}

bool AppViewModel::serverSearchLoading() const
{
    return m_serverSearchLoading;
}

bool AppViewModel::serverSearchHasMore() const
{
    return m_serverSearchHasMore;
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

QString AppViewModel::selectedItemId() const
{
    return m_selectedItem ? m_selectedItem->id : QString {};
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

QString AppViewModel::selectedItemLogoUrl() const
{
    return m_selectedItem ? m_selectedItem->logoImageUrl : QString {};
}

QString AppViewModel::selectedItemBackdropUrl() const
{
    const auto urls = selectedItemBackdropUrls();
    return urls.isEmpty() ? QString {} : urls.front();
}

QStringList AppViewModel::selectedItemBackdropUrls() const
{
    if (!m_selectedItem) {
        return {};
    }
    if (!m_selectedItem->backdropImageUrls.isEmpty()) {
        return m_selectedItem->backdropImageUrls;
    }
    if (isEpisodeItem(*m_selectedItem)) {
        return m_selectedItem->seriesImageUrl.isEmpty()
            ? QStringList {}
            : QStringList { m_selectedItem->seriesImageUrl };
    }
    return m_selectedItem->imageUrl.isEmpty()
        ? QStringList {}
        : QStringList { m_selectedItem->imageUrl };
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

int AppViewModel::currentPlaybackSubtitleStreamIndex() const
{
    return m_currentPlaybackSubtitleStreamIndex;
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

MediaItemListModel* AppViewModel::serverSearchResults()
{
    return &m_serverSearchResults;
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

PlaybackHistoryListModel* AppViewModel::globalPlaybackHistory()
{
    return &m_globalPlaybackHistory;
}

QString AppViewModel::globalHistoryFilter() const
{
    return m_globalHistoryFilter;
}

void AppViewModel::setGlobalHistoryFilter(const QString& value)
{
    const auto normalized = value.trimmed().isEmpty() ? QStringLiteral("All") : value.trimmed();
    if (m_globalHistoryFilter.compare(normalized, Qt::CaseInsensitive) == 0) {
        return;
    }
    m_globalHistoryFilter = normalized;
    emit globalHistoryFilterChanged();
    refreshGlobalHistory();
}

bool AppViewModel::globalHistoryHasMore() const
{
    return m_globalHistoryHasMore;
}

bool AppViewModel::globalHistoryLoading() const
{
    return m_globalHistoryLoading;
}

qint64 AppViewModel::historyTotalWatchSeconds() const
{
    return m_historyTotalWatchSeconds;
}

qint64 AppViewModel::historyTotalNetworkBytes() const
{
    return m_historyTotalNetworkBytes;
}

qint64 AppViewModel::historyTotalNetworkBytesIn() const
{
    return m_historyTotalNetworkBytesIn;
}

qint64 AppViewModel::historyTotalNetworkBytesOut() const
{
    return m_historyTotalNetworkBytesOut;
}

qint64 AppViewModel::historyNormalNetworkBytes() const
{
    return m_historyNormalNetworkBytes;
}

qint64 AppViewModel::historyNormalNetworkBytesIn() const
{
    return m_historyNormalNetworkBytesIn;
}

qint64 AppViewModel::historyNormalNetworkBytesOut() const
{
    return m_historyNormalNetworkBytesOut;
}

qint64 AppViewModel::historyKeepAliveNetworkBytes() const
{
    return m_historyKeepAliveNetworkBytes;
}

qint64 AppViewModel::historyKeepAliveNetworkBytesIn() const
{
    return m_historyKeepAliveNetworkBytesIn;
}

qint64 AppViewModel::historyKeepAliveNetworkBytesOut() const
{
    return m_historyKeepAliveNetworkBytesOut;
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

bool AppViewModel::missedScheduledPlaybackPromptVisible() const
{
    return missedScheduledPlaybackTaskCount() > 0;
}

int AppViewModel::missedScheduledPlaybackTaskCount() const
{
    return m_scheduledPlaybackManager.missedTaskCount();
}

QString AppViewModel::missedScheduledPlaybackMessage() const
{
    const auto count = missedScheduledPlaybackTaskCount();
    if (count <= 0) {
        return {};
    }

    auto names = m_scheduledPlaybackManager.missedTaskServerNames();
    const auto totalNameCount = static_cast<int>(names.size());
    const auto remainingNames = std::max(0, totalNameCount - 3);
    names = names.mid(0, std::min(3, totalNameCount));
    const auto chinese = effectiveLanguage(m_languageMode) == QStringLiteral("zh_CN");
    auto sourceText = names.join(chinese ? QStringLiteral("、") : QStringLiteral(", "));
    if (remainingNames > 0) {
        sourceText += chinese
            ? QStringLiteral(" 等 %1 个播放源").arg(remainingNames + names.size())
            : QStringLiteral(" and %1 more").arg(remainingNames);
    }

    if (chinese) {
        return sourceText.isEmpty()
            ? QStringLiteral("发现 %1 个保号策略在应用关闭期间错过运行，是否现在补跑？").arg(count)
            : QStringLiteral("发现 %1 个保号策略在应用关闭期间错过运行（%2），是否现在补跑？")
                  .arg(count)
                  .arg(sourceText);
    }
    return sourceText.isEmpty()
        ? QStringLiteral("%1 keep-alive %2 missed while the app was closed. Run them now?")
              .arg(count)
              .arg(count == 1 ? QStringLiteral("strategy was") : QStringLiteral("strategies were"))
        : QStringLiteral("%1 keep-alive %2 missed while the app was closed (%3). Run them now?")
              .arg(count)
              .arg(count == 1 ? QStringLiteral("strategy was") : QStringLiteral("strategies were"))
              .arg(sourceText);
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

QString AppViewModel::scheduledTaskScheduleType() const
{
    return m_scheduledTaskScheduleType;
}

void AppViewModel::setScheduledTaskScheduleType(const QString& value)
{
    const auto normalized = ScheduledPlaybackSchedule::isSupportedType(value)
        ? value
        : QString::fromLatin1(ScheduledPlaybackSchedule::manualType);
    if (m_scheduledTaskScheduleType == normalized) {
        return;
    }
    m_scheduledTaskScheduleType = normalized;
    emit scheduledTaskEditorChanged();
}

int AppViewModel::scheduledTaskStartHour() const
{
    return m_scheduledTaskStartHour;
}

void AppViewModel::setScheduledTaskStartHour(int value)
{
    const auto normalized = std::clamp(value, 0, 23);
    if (m_scheduledTaskStartHour == normalized) {
        return;
    }
    m_scheduledTaskStartHour = normalized;
    emit scheduledTaskEditorChanged();
}

int AppViewModel::scheduledTaskStartMinute() const
{
    return m_scheduledTaskStartMinute;
}

void AppViewModel::setScheduledTaskStartMinute(int value)
{
    const auto normalized = std::clamp(value, 0, 59);
    if (m_scheduledTaskStartMinute == normalized) {
        return;
    }
    m_scheduledTaskStartMinute = normalized;
    emit scheduledTaskEditorChanged();
}

int AppViewModel::scheduledTaskWeekday() const
{
    return m_scheduledTaskWeekday;
}

void AppViewModel::setScheduledTaskWeekday(int value)
{
    const auto normalized = std::clamp(value, 1, 7);
    if (m_scheduledTaskWeekday == normalized) {
        return;
    }
    m_scheduledTaskWeekday = normalized;
    emit scheduledTaskEditorChanged();
}

int AppViewModel::scheduledTaskMonthDay() const
{
    return m_scheduledTaskMonthDay;
}

void AppViewModel::setScheduledTaskMonthDay(int value)
{
    const auto normalized = std::clamp(value, 1, 31);
    if (m_scheduledTaskMonthDay == normalized) {
        return;
    }
    m_scheduledTaskMonthDay = normalized;
    emit scheduledTaskEditorChanged();
}

QVariantList AppViewModel::scheduledTaskCustomMonthDays() const
{
    QVariantList values;
    values.reserve(m_scheduledTaskCustomMonthDays.size());
    for (const auto day : m_scheduledTaskCustomMonthDays) {
        values.push_back(day);
    }
    return values;
}

bool AppViewModel::scheduledTaskEnabled() const
{
    return m_scheduledTaskEnabled;
}

void AppViewModel::setScheduledTaskEnabled(bool value)
{
    if (m_scheduledTaskEnabled == value) {
        return;
    }
    m_scheduledTaskEnabled = value;
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
    refreshLocalMediaRoots();
    refreshLinkPlaybackHistory();
    refreshTsslPackages();
    refreshGlobalHistory();
    refreshPrivacyCards();
    refreshUsageStats();
    refreshScheduledEmbySources();
    refreshScheduledPlaybackTasks();
    setCurrentView(QStringLiteral("services"));
}

void AppViewModel::beginAddServiceCard()
{
    m_pendingHistoryReplay.reset();
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
    m_pendingHistoryReplay.reset();
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
        if (m_pendingHistoryReplay &&
            m_pendingHistoryReplay->source == PlaybackHistorySource::WebDav &&
            m_pendingHistoryReplay->serviceId == m_pendingServiceCard->server.id) {
            const auto historyItem = *m_pendingHistoryReplay;
            const auto card = *m_pendingServiceCard;
            m_pendingHistoryReplay.reset();
            startWebDavHistoryPlayback(card, password, historyItem);
            return;
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

void AppViewModel::openLocalMedia()
{
    clearError();
    clearCurrentPlayback();
    m_selectedItem.reset();
    clearSeriesDetails();
    syncSelectedPeople();
    clearLocalMediaDirectory();
    refreshLocalMediaRoots();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("local"));
}

void AppViewModel::chooseLocalMediaRoot()
{
    clearError();
    const auto selected = QFileDialog::getExistingDirectory(
        nullptr,
        trText(QStringLiteral("local.addFolder")),
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation));
    if (selected.isEmpty()) {
        return;
    }

    const QFileInfo selectedInfo(selected);
    const auto canonicalPath = selectedInfo.canonicalFilePath();
    if (canonicalPath.isEmpty() || !selectedInfo.isDir() || !selectedInfo.isReadable()) {
        setError(trText(QStringLiteral("local.folderUnavailable")));
        return;
    }

    const auto normalizedPath = QDir::cleanPath(canonicalPath);
    const auto id = localMediaRootIdFor(normalizedPath);
    for (int row = 0; row < m_localMediaRoots.count(); ++row) {
        const auto existing = m_localMediaRoots.rootAt(row);
        if (existing && existing->id == id) {
            openLocalMediaRoot(row);
            return;
        }
    }

    auto displayName = QFileInfo(normalizedPath).fileName();
    if (displayName.isEmpty()) {
        displayName = QDir(normalizedPath).dirName();
    }
    if (displayName.isEmpty()) {
        displayName = QDir::toNativeSeparators(normalizedPath);
    }

    LocalMediaRoot root {
        .id = id,
        .name = displayName,
        .path = normalizedPath,
        .sortOrder = m_localMediaRoots.count(),
        .available = true,
    };
    if (auto result = m_repository.saveLocalMediaRoot(root); !result) {
        setError(result.error());
        return;
    }

    AppLogger::info(QStringLiteral("local-media"), QStringLiteral("Added a local media folder"));
    refreshLocalMediaRoots();
    for (int row = 0; row < m_localMediaRoots.count(); ++row) {
        const auto saved = m_localMediaRoots.rootAt(row);
        if (saved && saved->id == id) {
            openLocalMediaRoot(row);
            break;
        }
    }
}

void AppViewModel::openLocalMediaRoot(int row)
{
    clearError();
    const auto root = m_localMediaRoots.rootAt(row);
    if (!root) {
        return;
    }
    if (!root->available) {
        setError(trText(QStringLiteral("local.folderUnavailable")));
        return;
    }
    m_currentLocalMediaRoot = *root;
    emit localMediaDirectoryChanged();
    loadLocalMediaDirectory(root->path);
}

void AppViewModel::deleteLocalMediaRoot(int row)
{
    clearError();
    const auto root = m_localMediaRoots.rootAt(row);
    if (!root) {
        return;
    }
    if (auto result = m_repository.deleteLocalMediaRoot(root->id); !result) {
        setError(result.error());
        return;
    }
    if (m_currentLocalMediaRoot && m_currentLocalMediaRoot->id == root->id) {
        clearLocalMediaDirectory();
    }
    refreshLocalMediaRoots();
    AppLogger::info(QStringLiteral("local-media"), QStringLiteral("Removed a local media folder configuration"));
}

void AppViewModel::openLocalMediaItem(int row)
{
    clearError();
    const auto item = m_localMediaItems.itemAt(row);
    if (!item) {
        return;
    }
    if (!localMediaPathIsInsideRoot(item->path)) {
        setError(trText(QStringLiteral("local.folderUnavailable")));
        return;
    }
    if (item->directory) {
        loadLocalMediaDirectory(item->path);
        return;
    }
    if (!QFileInfo::exists(item->path) || !LocalMediaService::isSupportedVideoFile(item->path)) {
        setError(trText(QStringLiteral("local.fileUnavailable")));
        return;
    }

    startLocalVideoPlayback(item->path, item->name, true);
    AppLogger::info(QStringLiteral("local-media"), QStringLiteral("Opening local video playback"));
}

bool AppViewModel::openDroppedLocalVideo(const QUrl& url, double replacedPositionSeconds)
{
    clearError();
    const auto resolved = LocalMediaService::resolveVideoFile(url);
    if (!resolved) {
        setError(trText(QStringLiteral("local.dropUnsupported")));
        AppLogger::warning(QStringLiteral("local-media"), QStringLiteral("Rejected a dropped local video URL"));
        return false;
    }

    const auto stopPosition = m_currentPlaybackUrl.isEmpty()
        ? -1.0
        : std::max(0.0, replacedPositionSeconds);
    startLocalVideoPlayback(*resolved, QFileInfo(*resolved).fileName(), false, stopPosition);
    AppLogger::info(QStringLiteral("local-media"), QStringLiteral("Opening dropped local video playback"));
    return true;
}

void AppViewModel::startLocalVideoPlayback(const QString& path,
                                           const QString& displayName,
                                           bool retainLocalDirectory,
                                           double replacedPositionSeconds,
                                           double startPositionSeconds)
{
    clearCurrentPlayback(replacedPositionSeconds);
    if (!retainLocalDirectory) {
        clearLocalMediaDirectory();
    }

    setForegroundPlaybackActive(true);
    m_playbackOrigin = PlaybackOrigin::Local;
    m_currentPlaybackUrl = QUrl::fromLocalFile(path);
    m_currentIptvChannelId.clear();
    m_currentMediaSourceId.clear();
    m_currentPlaySessionId.clear();
    m_playbackHttpUsername.clear();
    m_playbackHttpPassword.clear();
    m_playbackAllowInsecureTls = false;
    m_currentPlaybackStartSeconds = std::max(0.0, startPositionSeconds);
    m_lastPlaybackReportSeconds = -1.0;
    m_playbackStartedReported = false;

    MediaItem media;
    media.id = path;
    media.name = displayName;
    media.itemType = QStringLiteral("Local Video");
    m_selectedItem = std::move(media);
    clearSeriesDetails();
    syncSelectedPeople();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("player"));
}

void AppViewModel::localMediaBack()
{
    if (!m_currentLocalMediaRoot || m_localMediaCurrentPath.isEmpty()) {
        backToServices();
        return;
    }

    auto current = QDir::fromNativeSeparators(QDir::cleanPath(m_localMediaCurrentPath));
    const auto root = QDir::fromNativeSeparators(QDir::cleanPath(m_currentLocalMediaRoot->path));
#ifdef Q_OS_WIN
    const auto atRoot = current.compare(root, Qt::CaseInsensitive) == 0;
#else
    const auto atRoot = current == root;
#endif
    if (atRoot) {
        clearLocalMediaDirectory();
        return;
    }

    QDir parent(current);
    if (!parent.cdUp() || !localMediaPathIsInsideRoot(parent.absolutePath())) {
        clearLocalMediaDirectory();
        return;
    }
    loadLocalMediaDirectory(parent.absolutePath());
}

void AppViewModel::refreshLocalMediaDirectory()
{
    if (m_currentLocalMediaRoot && !m_localMediaCurrentPath.isEmpty()) {
        loadLocalMediaDirectory(m_localMediaCurrentPath);
    } else {
        refreshLocalMediaRoots();
    }
}

void AppViewModel::openLinkPlayback()
{
    clearError();
    clearCurrentPlayback();
    refreshLinkPlaybackHistory();
    m_selectedItem.reset();
    clearSeriesDetails();
    syncSelectedPeople();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("link"));
}

bool AppViewModel::playLink()
{
    clearError();
    const auto playbackUrl = LinkPlaybackService::resolvePlaybackUrl(m_linkPlaybackAddress);
    if (!playbackUrl) {
        setError(trText(linkPlaybackErrorKey(playbackUrl.error())));
        AppLogger::warning(QStringLiteral("link-playback"), QStringLiteral("Rejected an invalid playback link"));
        return false;
    }

    return startLinkPlayback(*playbackUrl);
}

bool AppViewModel::playLinkHistory(const QString& recordId)
{
    clearError();
    const auto* historyItem = m_linkPlaybackHistory.itemById(recordId);
    if (!historyItem) {
        refreshLinkPlaybackHistory();
        setError(trText(QStringLiteral("link.historyInvalid")));
        return false;
    }

    const auto playbackUrl = LinkPlaybackService::resolvePlaybackUrl(
        historyItem->playbackUrl.toString(QUrl::FullyEncoded));
    if (!playbackUrl) {
        setError(trText(QStringLiteral("link.historyInvalid")));
        AppLogger::warning(QStringLiteral("link-playback"), QStringLiteral("Rejected an invalid saved playback link"));
        return false;
    }

    setLinkPlaybackAddress(playbackUrl->toString(QUrl::FullyEncoded));
    return startLinkPlayback(*playbackUrl);
}

void AppViewModel::deleteLinkPlaybackHistory(const QString& recordId)
{
    clearError();
    if (auto result = m_repository.deletePlaybackHistory(recordId); !result) {
        AppLogger::warning(QStringLiteral("link-playback"),
                           QStringLiteral("Delete playback history failed: %1").arg(result.error()));
        setError(trText(QStringLiteral("link.historyDeleteFailed")));
        return;
    }
    refreshLinkPlaybackHistory();
    refreshGlobalHistory();
    AppLogger::info(QStringLiteral("link-playback"), QStringLiteral("Deleted one playback history record"));
}

bool AppViewModel::startLinkPlayback(const QUrl& playbackUrl, double startPositionSeconds)
{
    clearCurrentPlayback();
    setForegroundPlaybackActive(true);
    m_playbackOrigin = PlaybackOrigin::Link;
    m_currentPlaybackUrl = playbackUrl;
    m_currentIptvChannelId.clear();
    m_currentMediaSourceId.clear();
    m_currentPlaySessionId.clear();
    m_playbackHttpUsername.clear();
    m_playbackHttpPassword.clear();
    m_playbackAllowInsecureTls = false;
    m_currentPlaybackStartSeconds = std::max(0.0, startPositionSeconds);
    m_lastPlaybackReportSeconds = -1.0;
    m_playbackStartedReported = false;

    MediaItem item;
    item.id = QStringLiteral("link-playback");
    item.name = LinkPlaybackService::displayName(playbackUrl);
    item.itemType = trText(QStringLiteral("link.mediaType"));
    m_selectedItem = std::move(item);
    clearSeriesDetails();
    syncSelectedPeople();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("player"));
    AppLogger::info(QStringLiteral("link-playback"), QStringLiteral("Opening HTTP media playback"));
    return true;
}

void AppViewModel::recordLinkPlaybackHistory(const QString& recordId,
                                             const QUrl& playbackUrl,
                                             const QDateTime& playedAt)
{
    const LinkPlaybackHistoryItem item {
        .id = recordId,
        .playbackUrl = playbackUrl,
        .playedDate = playedAt.toLocalTime().date(),
        .playedAt = playedAt.toUTC(),
        .privacyMode = m_privacyMode,
    };
    if (auto result = m_repository.saveLinkPlaybackHistory(item); !result) {
        AppLogger::warning(QStringLiteral("link-playback"),
                           QStringLiteral("Save playback history failed: %1").arg(result.error()));
        return;
    }
    refreshLinkPlaybackHistory();
}

void AppViewModel::refreshLinkPlaybackHistory()
{
    const auto result = m_repository.loadLinkPlaybackHistory(m_privacyMode);
    if (!result) {
        AppLogger::warning(QStringLiteral("link-playback"),
                           QStringLiteral("Load playback history failed: %1").arg(result.error()));
        setError(trText(QStringLiteral("link.historyLoadFailed")));
        return;
    }

    std::vector<LinkPlaybackHistoryItem> visibleItems;
    visibleItems.reserve(result->size());
    for (auto item : *result) {
        const auto playbackUrl = LinkPlaybackService::resolvePlaybackUrl(
            item.playbackUrl.toString(QUrl::FullyEncoded));
        if (!playbackUrl || !item.playedDate.isValid() || !item.playedAt.isValid()) {
            AppLogger::warning(QStringLiteral("link-playback"), QStringLiteral("Ignored an invalid playback history record"));
            continue;
        }
        item.playbackUrl = *playbackUrl;
        item.displayName = LinkPlaybackService::displayName(*playbackUrl);
        item.displayAddress = LinkPlaybackService::displayAddress(*playbackUrl);
        visibleItems.push_back(std::move(item));
    }
    m_linkPlaybackHistory.setItems(std::move(visibleItems));
}

PlaybackHistorySource AppViewModel::selectedGlobalHistorySource() const
{
    return playbackHistorySourceFromString(m_globalHistoryFilter);
}

void AppViewModel::openGlobalHistory()
{
    ++m_globalHistoryReplayGeneration;
    m_pendingHistoryReplay.reset();
    clearError();
    clearSeriesDetails();
    clearCurrentPlayback();
    refreshGlobalHistory();
    emit playbackChanged();
    setCurrentView(QStringLiteral("globalHistory"));
}

void AppViewModel::refreshGlobalHistory()
{
    loadGlobalHistoryPage(true);
}

void AppViewModel::loadMoreGlobalHistory()
{
    if (!m_globalHistoryLoading && m_globalHistoryHasMore) {
        loadGlobalHistoryPage(false);
    }
}

void AppViewModel::loadGlobalHistoryPage(bool resetItems)
{
    if (m_globalHistoryLoading) {
        return;
    }
    m_globalHistoryLoading = true;
    emit globalHistoryStateChanged();
    if (resetItems) {
        m_globalHistoryNextStartIndex = 0;
        m_globalPlaybackHistory.clear();
    }

    const auto result = m_repository.loadPlaybackHistory(m_privacyMode,
                                                         m_globalHistoryNextStartIndex,
                                                         m_globalHistoryPageSize,
                                                         selectedGlobalHistorySource());
    if (!result) {
        m_globalHistoryLoading = false;
        m_globalHistoryHasMore = false;
        emit globalHistoryStateChanged();
        AppLogger::warning(QStringLiteral("global-history"),
                           QStringLiteral("Load playback history failed: %1").arg(result.error()));
        setError(trText(QStringLiteral("globalHistory.loadFailed")));
        return;
    }

    const auto loadedCount = static_cast<int>(result->size());
    auto items = prepareGlobalHistoryItems(std::move(*result));
    if (resetItems) {
        m_globalPlaybackHistory.setItems(std::move(items));
    } else {
        m_globalPlaybackHistory.appendItems(std::move(items));
    }
    m_globalHistoryNextStartIndex += loadedCount;
    m_globalHistoryHasMore = loadedCount == m_globalHistoryPageSize;
    m_globalHistoryLoading = false;
    emit globalHistoryStateChanged();
}

std::vector<PlaybackHistoryItem> AppViewModel::prepareGlobalHistoryItems(std::vector<PlaybackHistoryItem> items)
{
    QHash<QString, ServiceCard> serviceCards;
    if (const auto cards = m_repository.loadAllServiceCards(); cards) {
        for (const auto& card : *cards) {
            serviceCards.insert(card.server.id, card);
        }
    }

    std::vector<PlaybackHistoryItem> visibleItems;
    visibleItems.reserve(items.size());
    for (auto& item : items) {
        if (item.source == PlaybackHistorySource::Unknown || item.id.isEmpty() ||
            item.replayTarget.isEmpty() || !item.playedAt.isValid()) {
            AppLogger::warning(QStringLiteral("global-history"), QStringLiteral("Ignored an invalid playback history record"));
            continue;
        }

        const auto serviceCard = serviceCards.constFind(item.serviceId);
        if (serviceCard != serviceCards.cend()) {
            item.serviceName = serviceCard->server.name;
        }

        switch (item.source) {
        case PlaybackHistorySource::Local: {
            const QFileInfo fileInfo(item.replayTarget);
            item.available = fileInfo.exists() && fileInfo.isFile() && fileInfo.isReadable() &&
                LocalMediaService::isSupportedVideoFile(fileInfo.fileName());
            item.displayTarget = QDir::toNativeSeparators(item.replayTarget);
            if (item.title.trimmed().isEmpty()) {
                item.title = fileInfo.fileName();
            }
            if (item.serviceName.isEmpty()) {
                item.serviceName = trText(QStringLiteral("local.title"));
            }
            break;
        }
        case PlaybackHistorySource::Link: {
            const auto url = LinkPlaybackService::resolvePlaybackUrl(item.replayTarget);
            item.available = url.has_value();
            if (url) {
                item.displayTarget = LinkPlaybackService::displayAddress(*url);
                if (item.title.trimmed().isEmpty() || item.title == QStringLiteral("Link Playback")) {
                    item.title = LinkPlaybackService::displayName(*url);
                }
            }
            if (item.serviceName.isEmpty() || item.serviceName == QStringLiteral("Link Playback")) {
                item.serviceName = trText(QStringLiteral("link.title"));
            }
            break;
        }
        case PlaybackHistorySource::WebDav: {
            const QUrl target(item.replayTarget, QUrl::StrictMode);
            item.available = serviceCard != serviceCards.cend() && target.isValid() &&
                webDavHistoryTargetIsValid(serviceCard->server, target);
            auto displayUrl = target;
            displayUrl.setQuery(QString {});
            displayUrl.setFragment(QString {});
            displayUrl.setUserInfo(QString {});
            item.displayTarget = displayUrl.toString(QUrl::FullyDecoded);
            break;
        }
        case PlaybackHistorySource::Iptv:
        case PlaybackHistorySource::Emby:
        case PlaybackHistorySource::Jellyfin:
            item.available = serviceCard != serviceCards.cend();
            item.displayTarget = item.serviceName;
            break;
        case PlaybackHistorySource::Unknown:
            break;
        }

        if (item.subtitle.isEmpty()) {
            item.subtitle = playbackHistorySourceToString(item.source);
        }
        visibleItems.push_back(std::move(item));
    }
    return visibleItems;
}

std::optional<ServiceCard> AppViewModel::serviceCardForHistory(const QString& serviceId)
{
    const auto cards = m_repository.loadAllServiceCards();
    if (!cards) {
        setError(cards.error());
        return std::nullopt;
    }
    const auto card = std::ranges::find(*cards, serviceId, [](const ServiceCard& value) {
        return value.server.id;
    });
    return card == cards->cend() ? std::nullopt : std::optional<ServiceCard> { *card };
}

bool AppViewModel::webDavHistoryTargetIsValid(const ServerConfig& server, const QUrl& target) const
{
    if (!target.isValid() || target.isRelative() || !target.userInfo().isEmpty()) {
        return false;
    }
    const auto base = ensureDirectoryUrl(QUrl(server.baseUrl)).adjusted(QUrl::NormalizePathSegments);
    const auto normalizedTarget = target.adjusted(QUrl::NormalizePathSegments);
    const auto defaultPort = [](const QUrl& url) {
        return url.scheme().compare(QStringLiteral("https"), Qt::CaseInsensitive) == 0 ? 443 : 80;
    };
    if (!base.isValid() || normalizedTarget.scheme().compare(base.scheme(), Qt::CaseInsensitive) != 0 ||
        normalizedTarget.host().compare(base.host(), Qt::CaseInsensitive) != 0 ||
        normalizedTarget.port(defaultPort(normalizedTarget)) != base.port(defaultPort(base))) {
        return false;
    }
    const auto basePath = ensureDirectoryUrl(base).path(QUrl::FullyEncoded);
    const auto targetPath = normalizedTarget.path(QUrl::FullyEncoded);
    return targetPath.startsWith(basePath);
}

bool AppViewModel::playGlobalHistory(const QString& recordId)
{
    ++m_globalHistoryReplayGeneration;
    m_pendingHistoryReplay.reset();
    clearError();
    const auto* storedItem = m_globalPlaybackHistory.itemById(recordId);
    if (!storedItem) {
        refreshGlobalHistory();
        setError(trText(QStringLiteral("globalHistory.invalid")));
        return false;
    }
    const auto item = *storedItem;
    const auto startPositionSeconds = item.completed ? 0 : item.positionSeconds;

    switch (item.source) {
    case PlaybackHistorySource::Local: {
        const auto resolved = LocalMediaService::resolveVideoFile(QUrl::fromLocalFile(item.replayTarget));
        if (!resolved) {
            setError(trText(QStringLiteral("globalHistory.localUnavailable")));
            return false;
        }
        startLocalVideoPlayback(*resolved, item.title, false, -1.0, startPositionSeconds);
        return true;
    }
    case PlaybackHistorySource::Link: {
        const auto playbackUrl = LinkPlaybackService::resolvePlaybackUrl(item.replayTarget);
        if (!playbackUrl) {
            setError(trText(QStringLiteral("globalHistory.linkExpired")));
            return false;
        }
        setLinkPlaybackAddress(playbackUrl->toString(QUrl::FullyEncoded));
        return startLinkPlayback(*playbackUrl, startPositionSeconds);
    }
    case PlaybackHistorySource::Iptv: {
        const auto card = serviceCardForHistory(item.serviceId);
        if (!card || card->server.serviceType != ServiceType::IPTV) {
            setError(trText(QStringLiteral("globalHistory.serviceUnavailable")));
            return false;
        }
        loadIptvService(*card);
        const auto channel = std::ranges::find(m_allIptvChannels, item.replayTarget, &IptvChannel::id);
        if (channel == m_allIptvChannels.cend()) {
            setError(trText(QStringLiteral("globalHistory.channelUnavailable")));
            return false;
        }
        startIptvChannelPlayback(*channel);
        return true;
    }
    case PlaybackHistorySource::WebDav: {
        const auto card = serviceCardForHistory(item.serviceId);
        const QUrl target(item.replayTarget, QUrl::StrictMode);
        if (!card || card->server.serviceType != ServiceType::WebDAV ||
            !webDavHistoryTargetIsValid(card->server, target)) {
            setError(trText(QStringLiteral("globalHistory.webDavUnavailable")));
            return false;
        }
        const auto password = loadWebDavPassword(card->server);
        if (!password) {
            m_pendingHistoryReplay = item;
            m_pendingServiceCard = *card;
            setServerUrl(card->server.baseUrl);
            setServerName(card->server.name);
            setUsername(card->server.username);
            setServiceType(serviceTypeToString(card->server.serviceType));
            emit passwordRequired(card->server.name, card->server.username);
            return true;
        }
        startWebDavHistoryPlayback(*card, *password, item);
        return true;
    }
    case PlaybackHistorySource::Emby:
    case PlaybackHistorySource::Jellyfin:
        return replayMediaServerHistory(item);
    case PlaybackHistorySource::Unknown:
        break;
    }

    setError(trText(QStringLiteral("globalHistory.invalid")));
    return false;
}

void AppViewModel::deleteGlobalHistory(const QString& recordId)
{
    clearError();
    const auto* item = m_globalPlaybackHistory.itemById(recordId);
    const auto source = item ? item->source : PlaybackHistorySource::Unknown;
    if (auto result = m_repository.deletePlaybackHistory(recordId); !result) {
        AppLogger::warning(QStringLiteral("global-history"),
                           QStringLiteral("Delete playback history failed: %1").arg(result.error()));
        setError(trText(QStringLiteral("globalHistory.deleteFailed")));
        return;
    }
    if (source == PlaybackHistorySource::Link) {
        refreshLinkPlaybackHistory();
    }
    refreshGlobalHistory();
}

void AppViewModel::cancelPendingHistoryReplay()
{
    ++m_globalHistoryReplayGeneration;
    m_pendingHistoryReplay.reset();
}

bool AppViewModel::replayMediaServerHistory(const PlaybackHistoryItem& historyItem)
{
    const auto card = serviceCardForHistory(historyItem.serviceId);
    const auto expectedType = historyItem.source == PlaybackHistorySource::Jellyfin
        ? ServiceType::Jellyfin
        : ServiceType::Emby;
    if (!card || card->server.serviceType != expectedType) {
        setError(trText(QStringLiteral("globalHistory.serviceUnavailable")));
        return false;
    }

    const auto sessionResult = m_repository.loadSession(card->server.id);
    if (!sessionResult) {
        setError(sessionResult.error());
        return false;
    }
    if (!sessionResult->has_value()) {
        m_pendingHistoryReplay = historyItem;
        m_pendingServiceCard = *card;
        setServerUrl(card->server.baseUrl);
        setServerName(card->server.name);
        setUsername(card->server.username);
        setServiceType(serviceTypeToString(card->server.serviceType));
        setTrustSelfSignedCertificate(card->server.trustSelfSignedCertificate);
        setAutoLogin(card->server.autoLogin);
        emit passwordRequired(card->server.name, card->server.username);
        return true;
    }

    clearIptvState();
    clearWebDavState();
    setSession(**sessionResult);
    auto* client = clientFor(m_session->server.serviceType);
    if (!client) {
        setError(trText(QStringLiteral("globalHistory.serviceUnavailable")));
        return false;
    }

    const auto generation = ++m_globalHistoryReplayGeneration;
    const auto historyCopy = historyItem;
    setLoading(true);
    client->fetchItemDetails(*m_session,
                             historyItem.replayTarget,
                             [this, generation, historyCopy](std::expected<MediaItem, NetworkError> result) {
        if (generation != m_globalHistoryReplayGeneration) {
            return;
        }
        setLoading(false);
        if (!result) {
            if (result.error().kind == NetworkErrorKind::Http &&
                (result.error().httpStatus == 401 || result.error().httpStatus == 403)) {
                const auto card = serviceCardForHistory(historyCopy.serviceId);
                if (card) {
                    m_pendingHistoryReplay = historyCopy;
                    m_pendingServiceCard = *card;
                    emit passwordRequired(card->server.name, card->server.username);
                    return;
                }
            }
            AppLogger::warning(QStringLiteral("global-history"),
                               QStringLiteral("Fetch history media details failed: %1")
                                   .arg(displayNetworkError(result.error())));
            setError(displayNetworkError(result.error()));
            return;
        }

        result->playbackPositionTicks = historyCopy.completed
            ? 0
            : std::max<qint64>(0, historyCopy.positionSeconds) * playbackTicksPerSecond;
        m_selectedItem = std::move(*result);
        clearSeriesDetails();
        syncSelectedPeople();
        emit selectedItemChanged();
        playSelectedItem();
    });
    return true;
}

void AppViewModel::playIptvChannel(int row)
{
    clearError();
    const auto channel = m_iptvChannels.channelAt(row);
    if (!channel) {
        return;
    }

    startIptvChannelPlayback(*channel);
}

void AppViewModel::startIptvChannelPlayback(const IptvChannel& channel)
{
    clearCurrentPlayback();

    const auto playbackUrl = QUrl(channel.streamUrl);
    if (!playbackUrl.isValid() && !QFileInfo::exists(channel.streamUrl)) {
        setError(QStringLiteral("IPTV channel URL is invalid"));
        return;
    }

    setForegroundPlaybackActive(true);
    m_playbackOrigin = PlaybackOrigin::Iptv;
    m_currentPlaybackUrl = playbackUrl.scheme().isEmpty() ? QUrl::fromLocalFile(channel.streamUrl) : playbackUrl;
    m_currentIptvChannelId = channel.id;
    m_currentMediaSourceId.clear();
    m_currentPlaySessionId.clear();
    m_currentPlaybackStartSeconds = 0.0;
    m_lastPlaybackReportSeconds = -1.0;
    m_playbackStartedReported = false;

    MediaItem item;
    item.id = channel.id;
    item.name = channel.name;
    item.itemType = QStringLiteral("IPTV");
    item.imageUrl = channel.logoUrl;
    item.genres = channel.groupName;
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
        clearWebDavAudioPlayback();
        m_webDavHistory.push_back(m_webDavCurrentUrl);
        loadWebDavDirectory(ensureDirectoryUrl(item->url));
        return;
    }
    if (item->audioPlayable && m_webDavDisplayMode == QStringLiteral("audio")) {
        startWebDavAudioPlayback(row);
        return;
    }
    if (!item->playable) {
        return;
    }

    if (!m_currentWebDavCard) {
        return;
    }
    if (item->encryptedHls) {
        const auto generation = ++m_encryptedHlsPrepareGeneration;
        const auto serverId = m_currentWebDavCard->server.id;
        m_encryptedHlsPreparing = true;
        setLoading(true);
        m_encryptedHlsPlaybackProxy.prepareStream(
            m_currentWebDavCard->server,
            m_webDavPassword,
            item->url,
            [this, generation, serverId, item = *item](EncryptedHlsPrepareResult result) {
                const bool isCurrentGeneration = generation == m_encryptedHlsPrepareGeneration;
                if (!isCurrentGeneration || !m_currentWebDavCard ||
                    m_currentWebDavCard->server.id != serverId) {
                    if (result) {
                        m_encryptedHlsPlaybackProxy.revoke(result->sessionId);
                    }
                    if (isCurrentGeneration) {
                        m_encryptedHlsPreparing = false;
                        setLoading(false);
                    }
                    return;
                }
                m_encryptedHlsPreparing = false;
                setLoading(false);
                if (!result) {
                    setError(result.error());
                    return;
                }
                startWebDavVideoPlayback(item, result->url, result->sessionId);
            });
        return;
    }

    const auto proxyUrl = m_webDavPlaybackProxy.streamUrlFor(m_currentWebDavCard->server,
                                                              m_webDavPassword,
                                                              item->url);
    startWebDavVideoPlayback(*item, proxyUrl);
}

void AppViewModel::startWebDavVideoPlayback(const WebDavItem& item,
                                            const QUrl& proxyUrl,
                                            const QString& encryptedSessionId)
{
    clearCurrentPlayback();
    setForegroundPlaybackActive(true);
    m_playbackOrigin = PlaybackOrigin::WebDav;
    m_currentPlaybackUrl = proxyUrl;
    m_currentIptvChannelId.clear();
    if (encryptedSessionId.isEmpty()) {
        m_webDavPlaybackStreamId = proxyUrl.path().section(QLatin1Char('/'), 1, 1);
    } else {
        m_encryptedHlsPlaybackSessionId = encryptedSessionId;
    }
    m_playbackHttpUsername.clear();
    m_playbackHttpPassword.clear();
    m_playbackAllowInsecureTls = false;
    m_currentMediaSourceId.clear();
    m_currentPlaySessionId.clear();
    m_currentPlaybackStartSeconds = 0.0;
    m_lastPlaybackReportSeconds = -1.0;
    m_playbackStartedReported = false;

    MediaItem media;
    media.id = item.url.toString();
    media.name = item.name;
    media.itemType = QStringLiteral("WebDAV");
    media.overview = item.contentType;
    m_selectedItem = std::move(media);
    clearSeriesDetails();
    syncSelectedPeople();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("player"));
}

void AppViewModel::startWebDavHistoryPlayback(const ServiceCard& card,
                                              const QString& password,
                                              const PlaybackHistoryItem& historyItem)
{
    const QUrl remoteUrl(historyItem.replayTarget, QUrl::StrictMode);
    if (!webDavHistoryTargetIsValid(card.server, remoteUrl)) {
        setError(trText(QStringLiteral("globalHistory.webDavUnavailable")));
        return;
    }

    if (remoteUrl.path().endsWith(QStringLiteral(".m3u8s"), Qt::CaseInsensitive)) {
        const auto generation = ++m_encryptedHlsPrepareGeneration;
        m_encryptedHlsPreparing = true;
        setLoading(true);
        m_encryptedHlsPlaybackProxy.prepareStream(
            card.server,
            password,
            remoteUrl,
            [this, generation, card, password, historyItem, remoteUrl](EncryptedHlsPrepareResult result) {
                if (generation != m_encryptedHlsPrepareGeneration) {
                    if (result) {
                        m_encryptedHlsPlaybackProxy.revoke(result->sessionId);
                    }
                    return;
                }
                m_encryptedHlsPreparing = false;
                setLoading(false);
                if (!result) {
                    setError(result.error());
                    return;
                }
                finishWebDavHistoryPlayback(card,
                                            password,
                                            historyItem,
                                            remoteUrl,
                                            result->url,
                                            result->sessionId);
            });
        return;
    }

    const auto proxyUrl = m_webDavPlaybackProxy.streamUrlFor(card.server, password, remoteUrl);
    finishWebDavHistoryPlayback(card, password, historyItem, remoteUrl, proxyUrl);
}

void AppViewModel::finishWebDavHistoryPlayback(const ServiceCard& card,
                                               const QString& password,
                                               const PlaybackHistoryItem& historyItem,
                                               const QUrl& remoteUrl,
                                               const QUrl& proxyUrl,
                                               const QString& encryptedSessionId)
{
    clearCurrentPlayback();
    m_session.reset();
    clearIptvState();
    m_currentWebDavCard = card;
    m_webDavPassword = password;
    m_webDavHistory.clear();
    m_webDavCurrentUrl = ensureDirectoryUrl(QUrl(card.server.baseUrl));
    m_webDavDirectoryItems.clear();
    m_webDavAudioQueue.clear();
    m_webDavItems.clear();

    m_playbackOrigin = PlaybackOrigin::WebDav;
    m_currentPlaybackUrl = proxyUrl;
    m_currentIptvChannelId.clear();
    if (encryptedSessionId.isEmpty()) {
        m_webDavPlaybackStreamId = proxyUrl.path().section(QLatin1Char('/'), 1, 1);
    } else {
        m_encryptedHlsPlaybackSessionId = encryptedSessionId;
    }
    m_playbackHttpUsername.clear();
    m_playbackHttpPassword.clear();
    m_playbackAllowInsecureTls = false;
    m_currentMediaSourceId.clear();
    m_currentPlaySessionId.clear();
    m_currentPlaybackStartSeconds = historyItem.completed
        ? 0
        : std::max<qint64>(0, historyItem.positionSeconds);
    m_lastPlaybackReportSeconds = -1.0;
    m_playbackStartedReported = false;

    MediaItem media;
    media.id = remoteUrl.toString(QUrl::FullyEncoded);
    media.name = historyItem.title;
    const auto audioPlayback = historyItem.subtitle.contains(QStringLiteral("Audio"), Qt::CaseInsensitive);
    media.itemType = audioPlayback
        ? QStringLiteral("WebDAV Audio")
        : QStringLiteral("WebDAV");
    media.overview = historyItem.displayTarget;
    m_selectedItem = std::move(media);
    clearSeriesDetails();
    syncSelectedPeople();

    m_webDavAudioPlaybackActive = audioPlayback;
    m_webDavAudioCurrentIndex = audioPlayback ? 0 : -1;
    if (audioPlayback) {
        m_webDavAudioQueue.push_back(WebDavItem {
            .name = historyItem.title,
            .url = remoteUrl,
            .contentType = QStringLiteral("audio/*"),
            .playable = true,
            .audioPlayable = true,
        });
    }
    setForegroundPlaybackActive(true);
    emit loggedInChanged();
    emit currentUserChanged();
    emit currentServerChanged();
    emit webDavCurrentPathChanged();
    emit webDavAudioPlaybackChanged();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("player"));
    AppLogger::info(QStringLiteral("global-history"), QStringLiteral("Replaying a WebDAV history item"));
}

void AppViewModel::startWebDavAudioPlayback(int row)
{
    if (!m_currentWebDavCard) {
        return;
    }
    if (m_webDavAudioQueue.empty()) {
        rebuildWebDavAudioQueue(m_webDavDirectoryItems);
    }
    if (m_webDavAudioQueue.empty()) {
        return;
    }
    const auto index = row >= 0 ? row : 0;
    if (index >= static_cast<int>(m_webDavAudioQueue.size())) {
        return;
    }
    m_webDavAudioPlaybackActive = true;
    m_webDavAudioCurrentIndex = index;
    emit webDavAudioPlaybackChanged();
    playWebDavAudioTrack(index);
    setCurrentView(QStringLiteral("player"));
}

void AppViewModel::advanceWebDavAudioPlayback(bool reachedEnd, bool failed)
{
    if (!m_webDavAudioPlaybackActive || (!reachedEnd && !failed)) {
        return;
    }
    if (reachedEnd && m_webDavAudioRepeatMode == QStringLiteral("one")) {
        AppLogger::info(QStringLiteral("webdav"), QStringLiteral("Repeating current WebDAV audio item"));
        playWebDavAudioTrack(m_webDavAudioCurrentIndex);
        return;
    }

    auto nextIndex = m_webDavAudioCurrentIndex + 1;
    if (nextIndex >= static_cast<int>(m_webDavAudioQueue.size())) {
        if (reachedEnd && m_webDavAudioRepeatMode == QStringLiteral("all")) {
            nextIndex = 0;
            AppLogger::info(QStringLiteral("webdav"), QStringLiteral("Restarting WebDAV audio queue"));
        } else {
            AppLogger::info(QStringLiteral("webdav"), QStringLiteral("WebDAV audio queue completed"));
            clearWebDavAudioPlayback();
            if (m_currentWebDavCard) {
                setCurrentView(QStringLiteral("webdav"));
            }
            return;
        }
    }
    AppLogger::info(QStringLiteral("webdav"),
                    QStringLiteral("Advancing WebDAV audio queue to item %1").arg(nextIndex + 1));
    m_webDavAudioCurrentIndex = nextIndex;
    emit webDavAudioPlaybackChanged();
    playWebDavAudioTrack(nextIndex);
}

void AppViewModel::skipWebDavAudioTrack(int direction)
{
    if (!m_webDavAudioPlaybackActive || m_webDavAudioQueue.size() <= 1 || direction == 0) {
        return;
    }
    auto nextIndex = m_webDavAudioCurrentIndex + (direction > 0 ? 1 : -1);
    if (nextIndex < 0 || nextIndex >= static_cast<int>(m_webDavAudioQueue.size())) {
        if (m_webDavAudioRepeatMode != QStringLiteral("all")) {
            return;
        }
        nextIndex = direction > 0 ? 0 : static_cast<int>(m_webDavAudioQueue.size()) - 1;
    }
    m_webDavAudioCurrentIndex = nextIndex;
    emit webDavAudioPlaybackChanged();
    playWebDavAudioTrack(nextIndex);
}

void AppViewModel::minimizeWebDavAudioPlayer()
{
    if (!m_webDavAudioPlaybackActive || m_currentView != QStringLiteral("player")) {
        return;
    }
    setCurrentView(QStringLiteral("webdav"));
    AppLogger::info(QStringLiteral("webdav"), QStringLiteral("Minimized WebDAV audio player"));
}

void AppViewModel::restoreWebDavAudioPlayer()
{
    if (!m_webDavAudioPlaybackActive || m_currentPlaybackUrl.isEmpty()) {
        return;
    }
    setCurrentView(QStringLiteral("player"));
    AppLogger::info(QStringLiteral("webdav"), QStringLiteral("Restored WebDAV audio player"));
}

void AppViewModel::rebuildWebDavAudioQueue(const std::vector<WebDavItem>& items)
{
    m_webDavAudioQueue.clear();
    m_webDavAudioQueue.reserve(items.size());
    std::ranges::copy_if(items,
                         std::back_inserter(m_webDavAudioQueue),
                         [](const WebDavItem& item) {
        return item.audioPlayable;
    });
    if (m_webDavAudioCurrentIndex >= static_cast<int>(m_webDavAudioQueue.size())) {
        m_webDavAudioCurrentIndex = -1;
    }
    emit webDavAudioPlaybackChanged();
}

void AppViewModel::playWebDavAudioTrack(int index)
{
    if (!m_currentWebDavCard || index < 0 || index >= static_cast<int>(m_webDavAudioQueue.size())) {
        return;
    }
    const auto& item = m_webDavAudioQueue[static_cast<size_t>(index)];
    finishGlobalPlaybackHistory(m_currentPlaybackPositionSeconds, false);
    finishPlaybackUsageTracking();
    m_currentPlaybackPositionSeconds = 0.0;
    m_currentPlaybackDurationSeconds = 0.0;
    if (!m_webDavPlaybackStreamId.isEmpty()) {
        m_webDavPlaybackProxy.revoke(m_webDavPlaybackStreamId);
        m_webDavPlaybackStreamId.clear();
    }
    const auto proxyUrl = m_webDavPlaybackProxy.streamUrlFor(m_currentWebDavCard->server, m_webDavPassword, item.url);
    m_playbackOrigin = PlaybackOrigin::WebDav;
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
    media.id = item.url.toString();
    media.name = item.name;
    media.itemType = QStringLiteral("WebDAV Audio");
    media.overview = item.contentType;
    m_selectedItem = std::move(media);
    clearSeriesDetails();
    syncSelectedPeople();
    setForegroundPlaybackActive(true);
    emit selectedItemChanged();
    emit playbackChanged();
    AppLogger::info(QStringLiteral("webdav"),
                    QStringLiteral("Playing WebDAV audio item %1 of %2")
                        .arg(index + 1)
                        .arg(static_cast<int>(m_webDavAudioQueue.size())));
}

void AppViewModel::clearWebDavAudioPlayback()
{
    if (!m_webDavAudioPlaybackActive && m_webDavAudioCurrentIndex < 0) {
        return;
    }
    m_webDavAudioPlaybackActive = false;
    m_webDavAudioCurrentIndex = -1;
    clearCurrentPlayback();
    emit playbackChanged();
    emit webDavAudioPlaybackChanged();
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

void AppViewModel::restoreTssl()
{
    clearError();
    setWebDavTsslStatus({});
    const auto selected = QFileDialog::getOpenFileName(nullptr,
                                                       trText(QStringLiteral("webdav.tsslRestore")),
                                                       {},
                                                       QStringLiteral("TSSL key packages (*.tssl)"));
    if (selected.isEmpty()) {
        return;
    }
    const auto restored = m_tsslStore.restoreFromFile(selected);
    if (!restored) {
        setError(restored.error());
        return;
    }
    AppLogger::info(QStringLiteral("encrypted-hls"),
                    QStringLiteral("Restored a local TSSL key package"));
    refreshTsslPackages();
    setWebDavTsslStatus(trText(QStringLiteral("webdav.tsslRestored")));
}

void AppViewModel::exportWebDavTssl(int row)
{
    clearError();
    setWebDavTsslStatus({});
    const auto item = m_webDavItems.itemAt(row);
    if (!item || !item->encryptedHls || !m_currentWebDavCard) {
        return;
    }

    const auto serverId = m_currentWebDavCard->server.id;
    setLoading(true);
    m_encryptedHlsPlaybackProxy.resolveRootDigest(
        m_currentWebDavCard->server,
        m_webDavPassword,
        item->url,
        [this, serverId, itemName = item->name](EncryptedHlsDigestResult result) {
            setLoading(false);
            if (!m_currentWebDavCard || m_currentWebDavCard->server.id != serverId) {
                return;
            }
            if (!result) {
                setError(result.error());
                return;
            }

            auto exportDirectory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
            auto exportName = QFileInfo(itemName).completeBaseName() + QStringLiteral(".tssl");
            auto destination = QFileDialog::getSaveFileName(nullptr,
                                                            trText(QStringLiteral("webdav.tsslExport")),
                                                            QDir(exportDirectory).filePath(exportName),
                                                            QStringLiteral("TSSL key packages (*.tssl)"));
            if (destination.isEmpty()) {
                return;
            }
            if (!destination.endsWith(QStringLiteral(".tssl"), Qt::CaseInsensitive)) {
                destination += QStringLiteral(".tssl");
            }
            if (auto exported = m_tsslStore.exportByRootDigest(*result, destination); !exported) {
                setError(exported.error());
                return;
            }
            AppLogger::info(QStringLiteral("encrypted-hls"),
                            QStringLiteral("Exported a local TSSL key package"));
            setWebDavTsslStatus(trText(QStringLiteral("webdav.tsslExported")));
        });
}

void AppViewModel::openM3u8sManager()
{
    clearError();
    refreshTsslPackages();
    setCurrentView(QStringLiteral("m3u8sManager"));
}

void AppViewModel::refreshTsslPackages()
{
    auto packages = m_tsslStore.listPackages();
    if (!packages) {
        setError(packages.error());
        return;
    }
    m_tsslPackages.setPackages(std::move(*packages));
}

void AppViewModel::restoreManagedTssl()
{
    clearError();
    const auto selected = QFileDialog::getOpenFileName(
        nullptr,
        trText(QStringLiteral("m3u8s.importTssl")),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QStringLiteral("TSSL key packages (*.tssl)"));
    if (selected.isEmpty()) {
        return;
    }
    const auto restored = m_tsslStore.restoreFromFile(selected);
    if (!restored) {
        setError(restored.error());
        return;
    }
    refreshTsslPackages();
    m_m3u8sStatus = trText(QStringLiteral("m3u8s.restoredStatus"));
    emit m3u8sStatusChanged();
    AppLogger::info(QStringLiteral("encrypted-hls"), QStringLiteral("Imported a managed TSSL package"));
}

void AppViewModel::exportManagedTssl(int row)
{
    clearError();
    const auto package = m_tsslPackages.packageAt(row);
    if (!package) {
        setError(trText(QStringLiteral("m3u8s.invalidPackage")));
        return;
    }
    const auto suggestedName = QString::fromLatin1(package->rootManifestDigest.toHex().first(16)) +
        QStringLiteral(".tssl");
    auto destination = QFileDialog::getSaveFileName(
        nullptr,
        trText(QStringLiteral("m3u8s.exportTssl")),
        QDir(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)).filePath(suggestedName),
        QStringLiteral("TSSL key packages (*.tssl)"));
    if (destination.isEmpty()) {
        return;
    }
    if (!destination.endsWith(QStringLiteral(".tssl"), Qt::CaseInsensitive)) {
        destination += QStringLiteral(".tssl");
    }
    if (auto exported = m_tsslStore.exportByRootDigest(package->rootManifestDigest, destination); !exported) {
        setError(exported.error());
        return;
    }
    m_m3u8sStatus = trText(QStringLiteral("m3u8s.exportedStatus"));
    emit m3u8sStatusChanged();
    AppLogger::info(QStringLiteral("encrypted-hls"), QStringLiteral("Exported a managed TSSL package"));
}

void AppViewModel::deleteManagedTssl(int row)
{
    clearError();
    const auto package = m_tsslPackages.packageAt(row);
    if (!package) {
        setError(trText(QStringLiteral("m3u8s.invalidPackage")));
        return;
    }
    if (auto deleted = m_tsslStore.deleteByRootDigest(package->rootManifestDigest); !deleted) {
        setError(deleted.error());
        return;
    }
    refreshTsslPackages();
    m_m3u8sStatus = trText(QStringLiteral("m3u8s.deletedStatus"));
    emit m3u8sStatusChanged();
    AppLogger::info(QStringLiteral("encrypted-hls"), QStringLiteral("Deleted a managed TSSL package"));
}

void AppViewModel::chooseM3u8sVideo()
{
    clearError();
    if (m_m3u8sPackager.isRunning()) {
        return;
    }
    const auto source = QFileDialog::getOpenFileName(
        nullptr,
        trText(QStringLiteral("m3u8s.chooseVideo")),
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation),
        QStringLiteral("Video files (*.mp4 *.mkv *.mov *.avi *.webm *.m4v *.ts *.mts *.m2ts);;All files (*)"));
    if (source.isEmpty()) {
        return;
    }
    const auto outputDirectory = QFileDialog::getExistingDirectory(
        nullptr,
        trText(QStringLiteral("m3u8s.chooseOutput")),
        QFileInfo(source).absolutePath());
    if (outputDirectory.isEmpty()) {
        return;
    }

    m_m3u8sStatus = trText(QStringLiteral("m3u8s.processingStatus"));
    m_m3u8sLastOutputDirectory.clear();
    emit m3u8sStatusChanged();
    const auto started = m_m3u8sPackager.start(EncryptedHlsPackageRequest {
        .sourcePath = source,
        .outputDirectory = outputDirectory,
        .segmentDurationSeconds = m_m3u8sSegmentDuration,
    });
    if (!started) {
        m_m3u8sStatus = trText(QStringLiteral("m3u8s.failedStatus"));
        emit m3u8sStatusChanged();
        setError(started.error());
    }
}

void AppViewModel::cancelM3u8sPackaging()
{
    m_m3u8sPackager.cancel();
}

void AppViewModel::openM3u8sOutputDirectory()
{
    if (m_m3u8sLastOutputDirectory.isEmpty() ||
        !QDesktopServices::openUrl(QUrl::fromLocalFile(m_m3u8sLastOutputDirectory))) {
        setError(trText(QStringLiteral("m3u8s.openFolderFailed")));
    }
}

void AppViewModel::openTsslStorageDirectory()
{
    const auto directory = m_tsslStore.storageDirectory();
    if (directory.isEmpty() || !QDir().mkpath(directory) ||
        !QDesktopServices::openUrl(QUrl::fromLocalFile(directory))) {
        setError(trText(QStringLiteral("m3u8s.openFolderFailed")));
    }
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
    setTransferDetailFilter(QStringLiteral("all"));
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
    setTransferDetailFilter(QStringLiteral("all"));
    m_transferManager.selectGroup(groupId);
}

void AppViewModel::closeTransferGroup()
{
    setTransferDetailFilter(QStringLiteral("all"));
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
    refreshGlobalHistory();
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
    refreshGlobalHistory();
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
    refreshGlobalHistory();
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
    clearServerSearchState();
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
    clearLocalMediaDirectory();
    m_currentLibrary.reset();
    clearMediaDirectoryState();
    clearServerSearchState();
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
    clearServerSearchState();
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
    if (m_detailsReturnToSearch && !m_activeServerSearchTerm.isEmpty()) {
        m_detailsReturnToSearch = false;
        m_selectedItem.reset();
        clearSeriesDetails();
        syncSelectedPeople();
        clearCurrentPlayback();
        emit selectedItemChanged();
        emit playbackChanged();
        setCurrentView(QStringLiteral("search"));
        return;
    }

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
    m_scheduledTaskSourceIndex = m_scheduledEmbySources.count() > 0 ? 0 : -1;
    m_scheduledTaskDurationMinutes = 90;
    m_scheduledTaskScheduleType = QString::fromLatin1(ScheduledPlaybackSchedule::manualType);
    m_scheduledTaskStartHour = 12;
    m_scheduledTaskStartMinute = 0;
    m_scheduledTaskWeekday = QDate::currentDate().dayOfWeek();
    m_scheduledTaskMonthDay = QDate::currentDate().day();
    m_scheduledTaskCustomMonthDays = { QDate::currentDate().day() };
    m_scheduledTaskEnabled = true;
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
    m_scheduledTaskScheduleType = ScheduledPlaybackSchedule::isSupportedType(task->scheduleType)
        ? task->scheduleType
        : QString::fromLatin1(ScheduledPlaybackSchedule::manualType);
    const auto startTime = QTime::fromString(task->startTime, QStringLiteral("HH:mm"));
    m_scheduledTaskStartHour = startTime.isValid() ? startTime.hour() : 12;
    m_scheduledTaskStartMinute = startTime.isValid() ? startTime.minute() : 0;
    const auto weeklyDays = ScheduledPlaybackSchedule::parseDays(task->scheduleDays, 1, 7);
    const auto monthlyDays = ScheduledPlaybackSchedule::parseDays(task->scheduleDays, 1, 31);
    m_scheduledTaskWeekday = weeklyDays.isEmpty() ? QDate::currentDate().dayOfWeek() : weeklyDays.front();
    m_scheduledTaskMonthDay = monthlyDays.isEmpty() ? QDate::currentDate().day() : monthlyDays.front();
    m_scheduledTaskCustomMonthDays = monthlyDays.isEmpty()
        ? QList<int> { QDate::currentDate().day() }
        : monthlyDays;
    m_scheduledTaskEnabled = task->enabled;
    emit scheduledTaskEditorChanged();
}

bool AppViewModel::saveScheduledPlaybackTask()
{
    return saveScheduledPlaybackTaskInternal(false);
}

bool AppViewModel::saveAndRunScheduledPlaybackTask()
{
    return saveScheduledPlaybackTaskInternal(true);
}

void AppViewModel::toggleScheduledTaskCustomMonthDay(int day)
{
    if (day < 1 || day > 31) {
        return;
    }
    if (m_scheduledTaskCustomMonthDays.contains(day)) {
        m_scheduledTaskCustomMonthDays.removeAll(day);
    } else {
        m_scheduledTaskCustomMonthDays.push_back(day);
        std::ranges::sort(m_scheduledTaskCustomMonthDays);
    }
    emit scheduledTaskEditorChanged();
}

std::optional<ScheduledPlaybackTask> AppViewModel::scheduledPlaybackTaskFromEditor()
{
    const auto source = m_scheduledEmbySources.cardAt(m_scheduledTaskSourceIndex);
    if (!source || source->server.serviceType != ServiceType::Emby || !source->hasSession) {
        setError(trText(QStringLiteral("schedule.errorSource")));
        return std::nullopt;
    }

    auto startTime = QStringLiteral("manual");
    auto scheduleDays = QStringLiteral("");
    if (m_scheduledTaskScheduleType != QLatin1String(ScheduledPlaybackSchedule::manualType)) {
        startTime = QStringLiteral("%1:%2")
            .arg(m_scheduledTaskStartHour, 2, 10, QLatin1Char('0'))
            .arg(m_scheduledTaskStartMinute, 2, 10, QLatin1Char('0'));
    }
    if (m_scheduledTaskScheduleType == QLatin1String(ScheduledPlaybackSchedule::weeklyType)) {
        scheduleDays = ScheduledPlaybackSchedule::serializeDays({ m_scheduledTaskWeekday }, 1, 7);
    } else if (m_scheduledTaskScheduleType == QLatin1String(ScheduledPlaybackSchedule::monthlyType)) {
        scheduleDays = ScheduledPlaybackSchedule::serializeDays({ m_scheduledTaskMonthDay }, 1, 31);
    } else if (m_scheduledTaskScheduleType == QLatin1String(ScheduledPlaybackSchedule::customMonthlyType)) {
        scheduleDays = ScheduledPlaybackSchedule::serializeDays(m_scheduledTaskCustomMonthDays, 1, 31);
        if (scheduleDays.isEmpty()) {
            setError(trText(QStringLiteral("schedule.errorCustomDays")));
            return std::nullopt;
        }
    }

    auto lastRunDate = QStringLiteral("");
    for (auto row = 0; row < m_scheduledPlaybackTasks.count(); ++row) {
        const auto existing = m_scheduledPlaybackTasks.taskAt(row);
        if (existing && existing->id == m_scheduledTaskEditingId &&
            existing->scheduleType == m_scheduledTaskScheduleType &&
            existing->startTime == startTime && existing->scheduleDays == scheduleDays) {
            lastRunDate = existing->lastRunDate;
            break;
        }
    }

    return ScheduledPlaybackTask {
        .id = m_scheduledTaskEditingId.isEmpty()
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : m_scheduledTaskEditingId,
        .serverId = source->server.id,
        .serverName = source->server.name,
        .username = source->server.username,
        .scheduleType = m_scheduledTaskScheduleType,
        .startTime = startTime,
        .scheduleDays = scheduleDays,
        .durationMinutes = std::clamp(m_scheduledTaskDurationMinutes, 5, 720),
        .enabled = m_scheduledTaskEnabled,
        .lastRunDate = lastRunDate,
        .privateMode = source->server.privateMode,
    };
}

bool AppViewModel::saveScheduledPlaybackTaskInternal(bool runNow)
{
    clearError();
    if (runNow && (scheduledPlaybackActive() || scheduledPlaybackWaiting())) {
        setError(trText(QStringLiteral("schedule.errorBusy")));
        return false;
    }

    const auto task = scheduledPlaybackTaskFromEditor();
    if (!task) {
        return false;
    }
    if (auto result = m_repository.saveScheduledPlaybackTask(*task); !result) {
        setError(result.error());
        return false;
    }

    refreshScheduledPlaybackTasks();
    if (runNow) {
        m_scheduledPlaybackManager.runNow(*task);
    }
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

void AppViewModel::resolveMissedScheduledPlaybackTasks(bool runNow)
{
    m_scheduledPlaybackManager.resolveMissedTasks(runNow);
}

QString AppViewModel::formatScheduledPlaybackSchedule(const QString& scheduleType,
                                                      const QString& startTime,
                                                      const QString& scheduleDays) const
{
    if (scheduleType == QLatin1String(ScheduledPlaybackSchedule::manualType)) {
        return trText(QStringLiteral("schedule.manual"));
    }

    const auto time = QTime::fromString(startTime, QStringLiteral("HH:mm"));
    const auto timeText = time.isValid() ? time.toString(QStringLiteral("HH:mm")) : QStringLiteral("--:--");
    const auto chinese = effectiveLanguage(m_languageMode) == QStringLiteral("zh_CN");

    if (scheduleType == QLatin1String(ScheduledPlaybackSchedule::dailyType)) {
        return chinese
            ? QStringLiteral("每天 %1").arg(timeText)
            : QStringLiteral("Every day at %1").arg(timeText);
    }

    if (scheduleType == QLatin1String(ScheduledPlaybackSchedule::weeklyType)) {
        const auto days = ScheduledPlaybackSchedule::parseDays(scheduleDays, 1, 7);
        const auto weekday = days.isEmpty() ? 1 : days.front();
        const auto weekdayText = trText(QStringLiteral("schedule.weekday%1").arg(weekday));
        return chinese
            ? QStringLiteral("每周%1 %2").arg(weekdayText.mid(2), timeText)
            : QStringLiteral("Every %1 at %2").arg(weekdayText, timeText);
    }

    const auto days = ScheduledPlaybackSchedule::parseDays(scheduleDays, 1, 31);
    QStringList dayTexts;
    dayTexts.reserve(days.size());
    for (const auto day : days) {
        dayTexts.push_back(QString::number(day));
    }
    const auto joinedDays = dayTexts.join(chinese ? QStringLiteral("、") : QStringLiteral(", "));
    if (scheduleType == QLatin1String(ScheduledPlaybackSchedule::monthlyType)) {
        const auto dayText = dayTexts.isEmpty() ? QStringLiteral("1") : dayTexts.front();
        return chinese
            ? QStringLiteral("每月 %1 日 %2").arg(dayText, timeText)
            : QStringLiteral("Day %1 of every month at %2").arg(dayText, timeText);
    }
    if (scheduleType == QLatin1String(ScheduledPlaybackSchedule::customMonthlyType)) {
        return chinese
            ? QStringLiteral("每月 %1 日 · %2").arg(joinedDays, timeText)
            : QStringLiteral("Monthly on days %1 at %2").arg(joinedDays, timeText);
    }

    return trText(QStringLiteral("schedule.manual"));
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

void AppViewModel::searchMediaServer()
{
    if (!serverSearchAvailable()) {
        return;
    }

    const auto normalizedTerm = m_serverSearchText.trimmed();
    if (normalizedTerm.isEmpty()) {
        clearServerSearch();
        return;
    }

    clearError();
    ++m_serverSearchRequestGeneration;
    m_serverSearchLoading = false;
    m_serverSearchText = normalizedTerm;
    m_activeServerSearchTerm = normalizedTerm;
    m_detailsReturnToSearch = false;
    m_serverSearchResults.clear();
    m_serverSearchNextStartIndex = 0;
    m_serverSearchHasMore = true;
    emit serverSearchChanged();
    setCurrentView(QStringLiteral("search"));
    loadServerSearchResults(false);
}

void AppViewModel::clearServerSearch()
{
    clearServerSearchState();
    if (m_session) {
        setCurrentView(QStringLiteral("home"));
    }
}

void AppViewModel::loadMoreServerSearchResults()
{
    loadServerSearchResults(false);
}

void AppViewModel::openServerSearchItem(int row)
{
    const auto item = m_serverSearchResults.itemAt(row);
    if (!item) {
        return;
    }
    openMediaItemDetails(*item, true);
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

void AppViewModel::clearServerSearchState(bool clearText)
{
    ++m_serverSearchRequestGeneration;
    m_serverSearchLoading = false;
    m_activeServerSearchTerm.clear();
    m_serverSearchNextStartIndex = 0;
    m_serverSearchHasMore = false;
    m_detailsReturnToSearch = false;
    m_serverSearchResults.clear();
    if (clearText) {
        m_serverSearchText.clear();
    }
    emit serverSearchChanged();
}

void AppViewModel::loadServerSearchResults(bool resetItems)
{
    if (!serverSearchAvailable() || m_activeServerSearchTerm.isEmpty() || m_serverSearchLoading) {
        return;
    }
    if (!resetItems && !m_serverSearchHasMore) {
        return;
    }

    if (resetItems) {
        m_serverSearchResults.clear();
        m_serverSearchNextStartIndex = 0;
        m_serverSearchHasMore = true;
    }

    const auto requestTerm = m_activeServerSearchTerm;
    const auto requestStartIndex = m_serverSearchNextStartIndex;
    const auto generation = ++m_serverSearchRequestGeneration;
    m_serverSearchLoading = true;
    emit serverSearchChanged();
    AppLogger::info(QStringLiteral("media-search"),
                    QStringLiteral("Searching current media server from index %1").arg(requestStartIndex));

    auto* client = clientFor(m_session->server.serviceType);
    if (!client) {
        m_serverSearchLoading = false;
        m_serverSearchHasMore = false;
        emit serverSearchChanged();
        return;
    }

    client->searchVideoItems(*m_session,
                             requestTerm,
                             requestStartIndex,
                             m_serverSearchPageSize,
                             [this, requestTerm, requestStartIndex, generation](ItemResult result) {
        if (generation != m_serverSearchRequestGeneration || requestTerm != m_activeServerSearchTerm) {
            AppLogger::info(QStringLiteral("media-search"), QStringLiteral("Ignoring stale search result page"));
            return;
        }

        m_serverSearchLoading = false;
        if (!result) {
            m_serverSearchHasMore = false;
            emit serverSearchChanged();
            AppLogger::warning(QStringLiteral("media-search"),
                               QStringLiteral("Search failed: %1").arg(displayNetworkError(result.error())));
            setError(displayNetworkError(result.error()));
            return;
        }

        auto items = std::move(*result);
        mergeRecentPlaybackProgress(items);
        const auto count = static_cast<int>(items.size());
        const auto appendedCount = m_serverSearchResults.appendItems(std::move(items));
        m_serverSearchNextStartIndex = requestStartIndex + count;
        m_serverSearchHasMore = count >= m_serverSearchPageSize && appendedCount > 0;
        emit serverSearchChanged();
        AppLogger::info(QStringLiteral("media-search"),
                        QStringLiteral("Fetched %1 search items, appended %2 unique items")
                            .arg(count)
                            .arg(appendedCount));
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
    const auto wasEpisodeSwitching = m_episodeSwitching;
    ++m_episodeDetailRequestGeneration;
    ++m_seriesRequestGeneration;
    m_selectedSeason.reset();
    m_seriesSeasons.clear();
    m_seriesEpisodes.clear();
    emit selectedSeasonChanged();
    setEpisodeSwitching(false);
    if (wasEpisodeSwitching) {
        setLoading(false);
    }
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
    ++m_encryptedHlsPrepareGeneration;
    if (m_encryptedHlsPreparing) {
        m_encryptedHlsPreparing = false;
        setLoading(false);
    }
    if (stopPositionSeconds >= 0.0) {
        reportPlaybackStopped(stopPositionSeconds);
    } else {
        finishGlobalPlaybackHistory(m_currentPlaybackPositionSeconds, false);
        finishPlaybackUsageTracking();
    }
    m_currentPlaybackUrl = QUrl();
    m_currentIptvChannelId.clear();
    m_currentMediaSourceId.clear();
    m_currentPlaySessionId.clear();
    m_currentPlaybackSubtitleStreamIndex = -1;
    m_playbackHttpUsername.clear();
    m_playbackHttpPassword.clear();
    m_playbackAllowInsecureTls = false;
    if (!m_webDavPlaybackStreamId.isEmpty()) {
        m_webDavPlaybackProxy.revoke(m_webDavPlaybackStreamId);
        m_webDavPlaybackStreamId.clear();
    }
    if (!m_encryptedHlsPlaybackSessionId.isEmpty()) {
        m_encryptedHlsPlaybackProxy.revoke(m_encryptedHlsPlaybackSessionId);
        m_encryptedHlsPlaybackSessionId.clear();
    }
    m_currentPlaybackStartSeconds = 0.0;
    m_currentPlaybackPositionSeconds = 0.0;
    m_currentPlaybackDurationSeconds = 0.0;
    m_currentPlaybackHistoryId.clear();
    m_lastHistoryPersistedPositionSeconds = -1.0;
    m_lastHistoryPersistedAt = {};
    m_lastPlaybackReportSeconds = -1.0;
    m_playbackStartedReported = false;
    m_playbackOrigin = PlaybackOrigin::None;
    const auto audioStateChanged = m_webDavAudioPlaybackActive || m_webDavAudioCurrentIndex >= 0;
    m_webDavAudioPlaybackActive = false;
    m_webDavAudioCurrentIndex = -1;
    if (audioStateChanged) {
        QTimer::singleShot(0, this, [this]() {
            emit webDavAudioPlaybackChanged();
        });
    }
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

    m_detailsReturnToSearch = false;
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

    openMediaItemDetails(*item, false);
}

void AppViewModel::openMediaItemDetails(const MediaItem& item, bool returnToSearch)
{
    m_detailsReturnToSearch = returnToSearch;
    m_selectedItem = item;
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
    client->fetchItemDetails(*m_session, item.id, [this](std::expected<MediaItem, NetworkError> result) {
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

    const auto generation = ++m_episodeDetailRequestGeneration;
    if (m_session) {
        setEpisodeSwitching(true);
        setLoading(true);
    }
    m_selectedItem = episodeContext;
    clearCurrentPlayback();
    emit selectedItemChanged();
    emit playbackChanged();
    setCurrentView(QStringLiteral("details"));

    if (!m_session) {
        syncSelectedPeople();
        return;
    }
    auto* client = clientFor(m_session->server.serviceType);
    client->fetchItemDetails(*m_session, episodeContext.id, [this, episodeContext, generation](std::expected<MediaItem, NetworkError> result) {
        if (generation != m_episodeDetailRequestGeneration) {
            return;
        }
        if (!result) {
            syncSelectedPeople();
            setEpisodeSwitching(false);
            setLoading(false);
            setError(displayNetworkError(result.error()));
            return;
        }
        auto detail = std::move(*result);
        applyMissingEpisodeContext(detail, episodeContext);
        m_selectedItem = std::move(detail);
        syncSelectedPeople();
        emit selectedItemChanged();
        setEpisodeSwitching(false);
        setLoading(false);
        if (!selectedItemHasSeriesEpisodes()) {
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
    const auto allowInsecureTls = m_session->server.trustSelfSignedCertificate;
    const auto historyReplayGeneration = m_currentView == QStringLiteral("globalHistory")
        ? m_globalHistoryReplayGeneration
        : -1;
    AppLogger::info(QStringLiteral("player"), QStringLiteral("Fetching playback info for selected media item"));
    client->fetchPlaybackUrl(*m_session,
                             *m_selectedItem,
                             [this, itemName, allowInsecureTls, historyReplayGeneration](PlaybackUrlResult result) {
        setLoading(false);
        if (historyReplayGeneration >= 0 && historyReplayGeneration != m_globalHistoryReplayGeneration) {
            setForegroundPlaybackActive(false);
            return;
        }
        if (!result) {
            setForegroundPlaybackActive(false);
            const auto message = displayNetworkError(result.error());
            AppLogger::warning(QStringLiteral("player"), QStringLiteral("Fetch playback URL failed for %1: %2").arg(itemName, message));
            setError(message);
            return;
        }

        m_currentPlaybackUrl = result->url;
        m_playbackOrigin = PlaybackOrigin::MediaServer;
        m_currentIptvChannelId.clear();
        m_currentMediaSourceId = result->mediaSourceId;
        m_currentPlaySessionId = result->playSessionId;
        m_currentPlaybackSubtitleStreamIndex = result->subtitleStreamIndex;
        m_playbackAllowInsecureTls = allowInsecureTls;
        m_currentPlaybackStartSeconds = result->startSeconds;
        m_lastPlaybackReportSeconds = -1.0;
        m_playbackStartedReported = false;
        AppLogger::info(QStringLiteral("player"), QStringLiteral("Opening player for selected media item"));
        setCurrentView(QStringLiteral("player"));
        emit playbackChanged();
    });
}

void AppViewModel::recordGlobalPlaybackStarted()
{
    if (!m_currentPlaybackHistoryId.isEmpty() || m_playbackOrigin == PlaybackOrigin::None ||
        !m_selectedItem || m_currentPlaybackUrl.isEmpty()) {
        return;
    }
    if (m_playbackOrigin == PlaybackOrigin::Local && m_selectedItem->id == QStringLiteral("local-verification")) {
        return;
    }

    PlaybackHistoryItem historyItem;
    historyItem.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    historyItem.title = m_selectedItem->name.trimmed();
    historyItem.subtitle = m_selectedItem->itemType.trimmed();
    historyItem.positionSeconds = std::max<qint64>(0, static_cast<qint64>(m_currentPlaybackStartSeconds));
    historyItem.durationSeconds = m_selectedItem->runTimeTicks > 0
        ? m_selectedItem->runTimeTicks / playbackTicksPerSecond
        : std::max<qint64>(0, static_cast<qint64>(m_currentPlaybackDurationSeconds));
    const auto localNow = QDateTime::currentDateTime();
    historyItem.playedDate = localNow.date();
    historyItem.playedAt = localNow.toUTC();
    historyItem.updatedAt = historyItem.playedAt;

    switch (m_playbackOrigin) {
    case PlaybackOrigin::MediaServer:
        if (!m_session || m_selectedItem->id.isEmpty()) {
            return;
        }
        historyItem.source = m_session->server.serviceType == ServiceType::Jellyfin
            ? PlaybackHistorySource::Jellyfin
            : PlaybackHistorySource::Emby;
        historyItem.serviceId = m_session->server.id;
        historyItem.serviceName = m_session->server.name;
        historyItem.replayTarget = m_selectedItem->id;
        historyItem.privacyMode = m_privacyMode || m_session->server.privateMode;
        if (!m_selectedItem->seriesName.isEmpty()) {
            historyItem.subtitle = m_selectedItem->seriesName;
            const auto seasonEpisode = selectedItemSeasonEpisode();
            if (!seasonEpisode.isEmpty()) {
                historyItem.subtitle += QStringLiteral(" · ") + seasonEpisode;
            }
        }
        break;
    case PlaybackOrigin::Iptv:
        if (!m_currentIptvCard || m_currentIptvChannelId.isEmpty()) {
            return;
        }
        historyItem.source = PlaybackHistorySource::Iptv;
        historyItem.serviceId = m_currentIptvCard->server.id;
        historyItem.serviceName = m_currentIptvCard->server.name;
        historyItem.replayTarget = m_currentIptvChannelId;
        historyItem.subtitle = m_selectedItem->genres.isEmpty() ? QStringLiteral("IPTV") : m_selectedItem->genres;
        historyItem.privacyMode = m_privacyMode || m_currentIptvCard->server.privateMode;
        break;
    case PlaybackOrigin::WebDav:
        if (!m_currentWebDavCard || m_selectedItem->id.isEmpty()) {
            return;
        }
        historyItem.source = PlaybackHistorySource::WebDav;
        historyItem.serviceId = m_currentWebDavCard->server.id;
        historyItem.serviceName = m_currentWebDavCard->server.name;
        historyItem.replayTarget = m_selectedItem->id;
        historyItem.privacyMode = m_privacyMode || m_currentWebDavCard->server.privateMode;
        break;
    case PlaybackOrigin::Local:
        historyItem.source = PlaybackHistorySource::Local;
        historyItem.serviceName = trText(QStringLiteral("local.title"));
        historyItem.replayTarget = m_currentPlaybackUrl.toLocalFile();
        historyItem.privacyMode = m_privacyMode;
        break;
    case PlaybackOrigin::Link:
        historyItem.source = PlaybackHistorySource::Link;
        historyItem.serviceId = QStringLiteral("builtin-link-playback");
        historyItem.serviceName = trText(QStringLiteral("link.title"));
        historyItem.replayTarget = m_currentPlaybackUrl.toString(QUrl::FullyEncoded);
        historyItem.privacyMode = m_privacyMode;
        break;
    case PlaybackOrigin::None:
        return;
    }

    if (historyItem.title.isEmpty() || historyItem.replayTarget.isEmpty()) {
        return;
    }
    if (auto result = m_repository.savePlaybackHistory(historyItem); !result) {
        AppLogger::warning(QStringLiteral("global-history"),
                           QStringLiteral("Save playback history failed: %1").arg(result.error()));
        return;
    }

    m_currentPlaybackHistoryId = historyItem.id;
    m_currentPlaybackPositionSeconds = m_currentPlaybackStartSeconds;
    m_lastHistoryPersistedPositionSeconds = m_currentPlaybackStartSeconds;
    m_lastHistoryPersistedAt = historyItem.updatedAt;
    if (historyItem.source == PlaybackHistorySource::Link) {
        recordLinkPlaybackHistory(historyItem.id, m_currentPlaybackUrl, historyItem.playedAt);
    }
    if (m_currentView == QStringLiteral("globalHistory")) {
        refreshGlobalHistory();
    }
    AppLogger::info(QStringLiteral("global-history"),
                    QStringLiteral("Recorded playback start for %1").arg(playbackHistorySourceToString(historyItem.source)));
}

void AppViewModel::updateGlobalPlaybackProgress(double positionSeconds,
                                                double durationSeconds,
                                                bool forceUpdate,
                                                bool completed)
{
    m_currentPlaybackPositionSeconds = std::max(0.0, positionSeconds);
    if (durationSeconds > 0.0) {
        m_currentPlaybackDurationSeconds = durationSeconds;
    }
    if (m_currentPlaybackHistoryId.isEmpty()) {
        return;
    }

    const auto now = QDateTime::currentDateTimeUtc();
    const auto positionDelta = std::abs(m_currentPlaybackPositionSeconds - m_lastHistoryPersistedPositionSeconds);
    const auto elapsedMs = m_lastHistoryPersistedAt.isValid() ? m_lastHistoryPersistedAt.msecsTo(now) : 0;
    if (!forceUpdate && positionDelta < 10.0 && elapsedMs < 15000) {
        return;
    }

    const auto position = static_cast<qint64>(m_currentPlaybackPositionSeconds);
    const auto duration = static_cast<qint64>(std::max(0.0, m_currentPlaybackDurationSeconds));
    const auto effectivelyCompleted = completed || (duration > 0 && position >= static_cast<qint64>(duration * 0.97));
    if (auto result = m_repository.updatePlaybackHistoryProgress(m_currentPlaybackHistoryId,
                                                                 position,
                                                                 duration,
                                                                 effectivelyCompleted,
                                                                 now);
        !result) {
        AppLogger::warning(QStringLiteral("global-history"),
                           QStringLiteral("Update playback history failed: %1").arg(result.error()));
        return;
    }
    m_lastHistoryPersistedPositionSeconds = m_currentPlaybackPositionSeconds;
    m_lastHistoryPersistedAt = now;
}

void AppViewModel::finishGlobalPlaybackHistory(double positionSeconds, bool completed)
{
    if (m_currentPlaybackHistoryId.isEmpty()) {
        return;
    }
    updateGlobalPlaybackProgress(positionSeconds, m_currentPlaybackDurationSeconds, true, completed);
    m_currentPlaybackHistoryId.clear();
    m_lastHistoryPersistedPositionSeconds = -1.0;
    m_lastHistoryPersistedAt = {};
}

void AppViewModel::reportPlaybackStarted()
{
    if (m_playbackOrigin == PlaybackOrigin::None) {
        return;
    }
    recordGlobalPlaybackStarted();
    if (m_playbackOrigin == PlaybackOrigin::Local || m_playbackOrigin == PlaybackOrigin::Link) {
        return;
    }
    beginPlaybackUsageTracking();
    if (m_playbackOrigin != PlaybackOrigin::MediaServer || !m_session || !m_selectedItem || m_playbackStartedReported) {
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

void AppViewModel::reportPlaybackProgress(double positionSeconds, double durationSeconds, bool paused)
{
    if (m_playbackOrigin == PlaybackOrigin::None) {
        return;
    }
    if (m_currentPlaybackHistoryId.isEmpty() && (durationSeconds > 0.0 || positionSeconds > 0.0)) {
        recordGlobalPlaybackStarted();
    }
    updateGlobalPlaybackProgress(positionSeconds, durationSeconds, paused);
    if (m_playbackOrigin == PlaybackOrigin::Local || m_playbackOrigin == PlaybackOrigin::Link) {
        return;
    }
    if (!m_playbackUsageActive) {
        beginPlaybackUsageTracking();
    }
    if (m_playbackUsageActive) {
        recordPlaybackUsageUntilNow();
        m_playbackUsagePaused = paused;
    }

    if (m_playbackOrigin != PlaybackOrigin::MediaServer || !m_session || !m_selectedItem || !m_playbackStartedReported) {
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
    finishGlobalPlaybackHistory(positionSeconds, false);
    finishPlaybackUsageTracking();
    if (m_playbackOrigin != PlaybackOrigin::MediaServer || !m_session || !m_selectedItem || !m_playbackStartedReported) {
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

void AppViewModel::reportPlaybackEnded(double positionSeconds, bool reachedEnd, bool failed)
{
    finishGlobalPlaybackHistory(positionSeconds, reachedEnd && !failed);
    reportPlaybackStopped(positionSeconds);
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
    const auto playbackOrigin = m_playbackOrigin;
    const auto hasLocalDirectory = m_currentLocalMediaRoot.has_value();
    clearCurrentPlayback();
    emit playbackChanged();
    if (playbackOrigin == PlaybackOrigin::Local) {
        setCurrentView(hasLocalDirectory ? QStringLiteral("local") : QStringLiteral("services"));
        return;
    }
    if (playbackOrigin == PlaybackOrigin::Link) {
        setCurrentView(QStringLiteral("link"));
        return;
    }
    if (m_currentWebDavCard) {
        setCurrentView(QStringLiteral("webdav"));
        if (m_webDavItems.count() == 0) {
            const auto directoryUrl = m_webDavCurrentUrl.isEmpty()
                ? ensureDirectoryUrl(QUrl(m_currentWebDavCard->server.baseUrl))
                : m_webDavCurrentUrl;
            loadWebDavDirectory(directoryUrl);
        }
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
    m_playbackOrigin = PlaybackOrigin::Local;
    m_currentPlaybackUrl = url;
    m_currentIptvChannelId.clear();
    m_currentMediaSourceId.clear();
    m_currentPlaySessionId.clear();
    m_currentPlaybackStartSeconds = 0.0;
    m_lastPlaybackReportSeconds = -1.0;
    m_playbackStartedReported = false;

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
    case ServiceType::Link:
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

void AppViewModel::refreshLocalMediaRoots()
{
    const auto result = m_repository.loadLocalMediaRoots();
    if (!result) {
        setError(result.error());
        return;
    }

    auto roots = *result;
    for (auto& root : roots) {
        const QFileInfo info(root.path);
        root.available = info.exists() && info.isDir() && info.isReadable();
    }
    m_localMediaRoots.setRoots(std::move(roots));
}

void AppViewModel::loadLocalMediaDirectory(const QString& path)
{
    if (!m_currentLocalMediaRoot || !localMediaPathIsInsideRoot(path)) {
        setError(trText(QStringLiteral("local.folderUnavailable")));
        return;
    }

    const QFileInfo pathInfo(path);
    const auto canonicalPath = pathInfo.canonicalFilePath();
    const auto normalizedPath = QDir::cleanPath(canonicalPath.isEmpty() ? pathInfo.absoluteFilePath() : canonicalPath);
    const auto requestGeneration = ++m_localMediaRequestGeneration;
    if (!m_localMediaLoading) {
        m_localMediaLoading = true;
        emit localMediaLoadingChanged();
    }

    m_localMediaService.browseDirectoryAsync(normalizedPath, [this, requestGeneration, normalizedPath](LocalMediaService::BrowseResult result) mutable {
        if (requestGeneration != m_localMediaRequestGeneration) {
            return;
        }
        m_localMediaLoading = false;
        emit localMediaLoadingChanged();
        if (!result) {
            m_localMediaItems.clear();
            setError(result.error());
            return;
        }

        m_localMediaCurrentPath = normalizedPath;
        m_localMediaItems.setItems(std::move(*result));
        emit localMediaDirectoryChanged();
        AppLogger::info(QStringLiteral("local-media"),
                        QStringLiteral("Loaded %1 local directory entries").arg(m_localMediaItems.count()));
    });
}

void AppViewModel::clearLocalMediaDirectory()
{
    ++m_localMediaRequestGeneration;
    m_currentLocalMediaRoot.reset();
    m_localMediaCurrentPath.clear();
    m_localMediaItems.clear();
    if (m_localMediaLoading) {
        m_localMediaLoading = false;
        emit localMediaLoadingChanged();
    }
    emit localMediaDirectoryChanged();
}

bool AppViewModel::localMediaPathIsInsideRoot(const QString& path) const
{
    if (!m_currentLocalMediaRoot) {
        return false;
    }

    const QFileInfo candidateInfo(path);
    const QFileInfo rootInfo(m_currentLocalMediaRoot->path);
    const auto candidateCanonical = candidateInfo.canonicalFilePath();
    const auto rootCanonical = rootInfo.canonicalFilePath();
    const auto candidate = QDir::fromNativeSeparators(QDir::cleanPath(
        candidateCanonical.isEmpty() ? candidateInfo.absoluteFilePath() : candidateCanonical));
    const auto root = QDir::fromNativeSeparators(QDir::cleanPath(
        rootCanonical.isEmpty() ? rootInfo.absoluteFilePath() : rootCanonical));
    const auto prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
#ifdef Q_OS_WIN
    return candidate.compare(root, Qt::CaseInsensitive) == 0 || candidate.startsWith(prefix, Qt::CaseInsensitive);
#else
    return candidate == root || candidate.startsWith(prefix);
#endif
}

void AppViewModel::refreshScheduledPlaybackTasks()
{
    const auto result = m_repository.loadScheduledPlaybackTasks(m_privacyMode);
    if (!result) {
        AppLogger::warning(QStringLiteral("scheduled-playback"),
                           QStringLiteral("Load scheduled tasks failed: %1").arg(result.error()));
        return;
    }
    m_scheduledPlaybackManager.setTasks(*result, m_privacyMode);
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
        if (m_pendingHistoryReplay && m_pendingHistoryReplay->serviceId == m_session->server.id) {
            const auto historyItem = *m_pendingHistoryReplay;
            m_pendingHistoryReplay.reset();
            replayMediaServerHistory(historyItem);
            return;
        }
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
    clearServerSearchState();
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
    if (!m_currentWebDavCard && m_webDavCurrentUrl.isEmpty() &&
        m_webDavItems.count() == 0 && m_webDavAudioQueue.empty()) {
        return;
    }
    m_currentWebDavCard.reset();
    m_webDavPassword.clear();
    m_webDavCurrentUrl = QUrl();
    m_webDavHistory.clear();
    clearWebDavAudioPlayback();
    m_webDavDirectoryItems.clear();
    m_webDavAudioQueue.clear();
    m_webDavAudioCurrentIndex = -1;
    emit webDavAudioPlaybackChanged();
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
        m_webDavDirectoryItems = *result;
        m_webDavItems.setItems(std::move(*result));
        m_webDavItems.setDisplayMode(m_webDavDisplayMode);
        rebuildWebDavAudioQueue(m_webDavDirectoryItems);
        emit webDavCurrentPathChanged();
        setCurrentView(QStringLiteral("webdav"));
        if (m_webDavDisplayMode == QStringLiteral("audio") &&
            !m_webDavAudioPlaybackActive && !m_webDavAudioQueue.empty()) {
            startWebDavAudioPlayback();
        }
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
    connect(&m_encryptedHlsPlaybackProxy,
            &EncryptedHlsPlaybackProxy::networkTrafficSample,
            this,
            wireWebDavTraffic);
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
    qint64 normalNetworkBytesIn = 0;
    qint64 normalNetworkBytesOut = 0;
    qint64 keepAliveNetworkBytesIn = 0;
    qint64 keepAliveNetworkBytesOut = 0;
    for (const auto& stat : *result) {
        watchSeconds += stat.watchSeconds;
        normalNetworkBytesIn += stat.networkBytesIn;
        normalNetworkBytesOut += stat.networkBytesOut;
        keepAliveNetworkBytesIn += stat.keepAliveNetworkBytesIn;
        keepAliveNetworkBytesOut += stat.keepAliveNetworkBytesOut;
    }

    const auto normalNetworkBytes = normalNetworkBytesIn + normalNetworkBytesOut;
    const auto keepAliveNetworkBytes = keepAliveNetworkBytesIn + keepAliveNetworkBytesOut;
    m_historyTotalWatchSeconds = watchSeconds;
    m_historyNormalNetworkBytesIn = normalNetworkBytesIn;
    m_historyNormalNetworkBytesOut = normalNetworkBytesOut;
    m_historyNormalNetworkBytes = normalNetworkBytes;
    m_historyKeepAliveNetworkBytesIn = keepAliveNetworkBytesIn;
    m_historyKeepAliveNetworkBytesOut = keepAliveNetworkBytesOut;
    m_historyKeepAliveNetworkBytes = keepAliveNetworkBytes;
    m_historyTotalNetworkBytesIn = normalNetworkBytesIn + keepAliveNetworkBytesIn;
    m_historyTotalNetworkBytesOut = normalNetworkBytesOut + keepAliveNetworkBytesOut;
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
    if (m_playbackOrigin == PlaybackOrigin::MediaServer && m_session) {
        return m_session->server;
    }
    if (m_playbackOrigin == PlaybackOrigin::Iptv && m_currentIptvCard) {
        return m_currentIptvCard->server;
    }
    if (m_playbackOrigin == PlaybackOrigin::WebDav && m_currentWebDavCard) {
        return m_currentWebDavCard->server;
    }
    if (m_playbackOrigin == PlaybackOrigin::Link) {
        return linkPlaybackUsageServer();
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
    m_serverSearchResults.updatePlaybackProgress(itemId, normalizedTicks, playedPercentage, played);
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
    if (m_currentView == QStringLiteral("globalHistory") &&
        view != QStringLiteral("globalHistory") && view != QStringLiteral("player")) {
        ++m_globalHistoryReplayGeneration;
        m_pendingHistoryReplay.reset();
        setLoading(false);
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

void AppViewModel::setEpisodeSwitching(bool value)
{
    if (m_episodeSwitching == value) {
        return;
    }
    m_episodeSwitching = value;
    emit episodeSwitchingChanged();
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

void AppViewModel::setWebDavTsslStatus(QString message)
{
    if (m_webDavTsslStatus == message) {
        return;
    }
    m_webDavTsslStatus = std::move(message);
    emit webDavTsslStatusChanged();
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
