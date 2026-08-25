#pragma once

#include "database/SessionRepository.h"
#include "models/IptvChannel.h"
#include "models/IptvPlaylist.h"
#include "models/UserSession.h"
#include "network/NetworkClient.h"
#include "services/encryptedhls/EncryptedHlsBatchPackager.h"
#include "services/webdav/TransferManager.h"
#include "services/webdav/WebDavClient.h"
#include "services/webdav/WebDavDownloadPlanner.h"
#include "services/webdav/EncryptedHlsPlaybackProxy.h"
#include "services/webdav/TsslStore.h"
#include "services/webdav/WebDavPlaybackProxy.h"
#include "services/emby/EmbyClient.h"
#include "services/backup/TsslBackupService.h"
#include "services/jellyfin/JellyfinClient.h"
#include "services/local/LocalMediaService.h"
#include "services/scheduler/ScheduledPlaybackManager.h"
#include "services/update/UpdateService.h"
#include "utils/ErrorPresentation.h"
#include "viewmodels/IptvChannelListModel.h"
#include "viewmodels/LocalMediaItemListModel.h"
#include "viewmodels/LocalMediaRootListModel.h"
#include "viewmodels/LinkPlaybackHistoryListModel.h"
#include "viewmodels/DailyUsageStatsListModel.h"
#include "viewmodels/MediaItemListModel.h"
#include "viewmodels/MediaLibraryListModel.h"
#include "viewmodels/PersonListModel.h"
#include "viewmodels/PlaybackHistoryListModel.h"
#include "viewmodels/ServiceCardListModel.h"
#include "viewmodels/ScheduledPlaybackTaskListModel.h"
#include "viewmodels/TsslPackageListModel.h"
#include "viewmodels/WebDavItemListModel.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QTemporaryDir>
#include <QUrl>
#include <QVariantList>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <vector>


class AppViewModel final : public QObject {
    Q_OBJECT

    Q_PROPERTY(QString serverUrl READ serverUrl WRITE setServerUrl NOTIFY serverUrlChanged)
    Q_PROPERTY(QString serverName READ serverName WRITE setServerName NOTIFY serverNameChanged)
    Q_PROPERTY(QString username READ username WRITE setUsername NOTIFY usernameChanged)
    Q_PROPERTY(QString password READ password WRITE setPassword NOTIFY passwordChanged)
    Q_PROPERTY(QString serviceType READ serviceType WRITE setServiceType NOTIFY serviceTypeChanged)
    Q_PROPERTY(bool trustSelfSignedCertificate READ trustSelfSignedCertificate WRITE setTrustSelfSignedCertificate NOTIFY trustSelfSignedCertificateChanged)
    Q_PROPERTY(bool autoLogin READ autoLogin WRITE setAutoLogin NOTIFY autoLoginChanged)
    Q_PROPERTY(QString iptvFilePath READ iptvFilePath WRITE setIptvFilePath NOTIFY iptvFilePathChanged)
    Q_PROPERTY(QString iptvSearchText READ iptvSearchText WRITE setIptvSearchText NOTIFY iptvSearchTextChanged)
    Q_PROPERTY(QString iptvSelectedGroup READ iptvSelectedGroup NOTIFY iptvSelectedGroupChanged)
    Q_PROPERTY(QStringList iptvGroups READ iptvGroups NOTIFY iptvGroupsChanged)
    Q_PROPERTY(IptvChannelListModel* iptvChannels READ iptvChannels CONSTANT)
    Q_PROPERTY(bool iptvPlaybackActive READ iptvPlaybackActive NOTIFY playbackChanged)
    Q_PROPERTY(QString currentIptvChannelId READ currentIptvChannelId NOTIFY playbackChanged)
    Q_PROPERTY(LocalMediaRootListModel* localMediaRoots READ localMediaRoots CONSTANT)
    Q_PROPERTY(LocalMediaItemListModel* localMediaItems READ localMediaItems CONSTANT)
    Q_PROPERTY(QString localMediaCurrentPath READ localMediaCurrentPath NOTIFY localMediaDirectoryChanged)
    Q_PROPERTY(QString localMediaRootName READ localMediaRootName NOTIFY localMediaDirectoryChanged)
    Q_PROPERTY(bool localMediaDirectoryOpen READ localMediaDirectoryOpen NOTIFY localMediaDirectoryChanged)
    Q_PROPERTY(bool localMediaLoading READ localMediaLoading NOTIFY localMediaLoadingChanged)
    Q_PROPERTY(QString linkPlaybackAddress READ linkPlaybackAddress WRITE setLinkPlaybackAddress NOTIFY linkPlaybackAddressChanged)
    Q_PROPERTY(LinkPlaybackHistoryListModel* linkPlaybackHistory READ linkPlaybackHistory CONSTANT)
    Q_PROPERTY(WebDavItemListModel* webDavItems READ webDavItems CONSTANT)
    Q_PROPERTY(QString webDavCurrentPath READ webDavCurrentPath NOTIFY webDavCurrentPathChanged)
    Q_PROPERTY(QString webDavDisplayMode READ webDavDisplayMode WRITE setWebDavDisplayMode NOTIFY webDavDisplayModeChanged)
    Q_PROPERTY(bool webDavShowM3u8sIdentifier READ webDavShowM3u8sIdentifier WRITE setWebDavShowM3u8sIdentifier NOTIFY webDavDisplaySettingsChanged)
    Q_PROPERTY(bool webDavShowM3u8sSourceFileName READ webDavShowM3u8sSourceFileName WRITE setWebDavShowM3u8sSourceFileName NOTIFY webDavDisplaySettingsChanged)
    Q_PROPERTY(QString webDavTsslStatus READ webDavTsslStatus NOTIFY webDavTsslStatusChanged)
    Q_PROPERTY(QString tsslBackupTarget READ tsslBackupTarget WRITE setTsslBackupTarget NOTIFY tsslBackupChanged)
    Q_PROPERTY(QString tsslBackupWebDavServiceId READ tsslBackupWebDavServiceId WRITE setTsslBackupWebDavServiceId NOTIFY tsslBackupChanged)
    Q_PROPERTY(QString tsslBackupWebDavPath READ tsslBackupWebDavPath WRITE setTsslBackupWebDavPath NOTIFY tsslBackupChanged)
    Q_PROPERTY(QVariantList tsslBackupWebDavServices READ tsslBackupWebDavServices NOTIFY tsslBackupChanged)
    Q_PROPERTY(QString tsslBackupS3Endpoint READ tsslBackupS3Endpoint WRITE setTsslBackupS3Endpoint NOTIFY tsslBackupChanged)
    Q_PROPERTY(QString tsslBackupS3Bucket READ tsslBackupS3Bucket WRITE setTsslBackupS3Bucket NOTIFY tsslBackupChanged)
    Q_PROPERTY(QString tsslBackupS3Region READ tsslBackupS3Region WRITE setTsslBackupS3Region NOTIFY tsslBackupChanged)
    Q_PROPERTY(QString tsslBackupS3Prefix READ tsslBackupS3Prefix WRITE setTsslBackupS3Prefix NOTIFY tsslBackupChanged)
    Q_PROPERTY(QString tsslBackupS3AccessKey READ tsslBackupS3AccessKey WRITE setTsslBackupS3AccessKey NOTIFY tsslBackupChanged)
    Q_PROPERTY(bool tsslBackupS3SecretConfigured READ tsslBackupS3SecretConfigured NOTIFY tsslBackupChanged)
    Q_PROPERTY(bool tsslBackupRunning READ tsslBackupRunning NOTIFY tsslBackupChanged)
    Q_PROPERTY(QString tsslBackupStatus READ tsslBackupStatus NOTIFY tsslBackupChanged)
    Q_PROPERTY(TsslPackageListModel* tsslPackages READ tsslPackages CONSTANT)
    Q_PROPERTY(TsslPackageListModel* tsslBatchPackages READ tsslBatchPackages CONSTANT)
    Q_PROPERTY(bool m3u8sPackaging READ m3u8sPackaging NOTIFY m3u8sPackagingChanged)
    Q_PROPERTY(double m3u8sPackagingProgress READ m3u8sPackagingProgress NOTIFY m3u8sPackagingChanged)
    Q_PROPERTY(QString m3u8sPackagingPhase READ m3u8sPackagingPhase NOTIFY m3u8sPackagingChanged)
    Q_PROPERTY(QString m3u8sStatus READ m3u8sStatus NOTIFY m3u8sStatusChanged)
    Q_PROPERTY(bool m3u8sBatchExporting READ m3u8sBatchExporting NOTIFY m3u8sStatusChanged)
    Q_PROPERTY(QString m3u8sLastOutputDirectory READ m3u8sLastOutputDirectory NOTIFY m3u8sStatusChanged)
    Q_PROPERTY(QStringList m3u8sSelectedSources READ m3u8sSelectedSources NOTIFY m3u8sSourceSelectionChanged)
    Q_PROPERTY(bool m3u8sFfmpegAvailable READ m3u8sFfmpegAvailable CONSTANT)
    Q_PROPERTY(int m3u8sSegmentDuration READ m3u8sSegmentDuration WRITE setM3u8sSegmentDuration NOTIFY m3u8sSegmentDurationChanged)
    Q_PROPERTY(QString m3u8sOutputDirectory READ m3u8sOutputDirectory NOTIFY m3u8sSettingsChanged)
    Q_PROPERTY(QString m3u8sOutputMode READ m3u8sOutputMode WRITE setM3u8sOutputMode NOTIFY m3u8sSettingsChanged)
    Q_PROPERTY(QString m3u8sWebDavServiceId READ m3u8sWebDavServiceId WRITE setM3u8sWebDavServiceId NOTIFY m3u8sSettingsChanged)
    Q_PROPERTY(QString m3u8sWebDavPath READ m3u8sWebDavPath NOTIFY m3u8sSettingsChanged)
    Q_PROPERTY(QString m3u8sFallbackDirectory READ m3u8sFallbackDirectory NOTIFY m3u8sSettingsChanged)
    Q_PROPERTY(bool m3u8sKeepSuccessfulLocal READ m3u8sKeepSuccessfulLocal WRITE setM3u8sKeepSuccessfulLocal NOTIFY m3u8sSettingsChanged)
    Q_PROPERTY(QVariantList m3u8sWebDavServices READ m3u8sWebDavServices NOTIFY m3u8sWebDavPickerChanged)
    Q_PROPERTY(WebDavItemListModel* m3u8sWebDavDirectories READ m3u8sWebDavDirectories CONSTANT)
    Q_PROPERTY(QString m3u8sWebDavPickerPath READ m3u8sWebDavPickerPath NOTIFY m3u8sWebDavPickerChanged)
    Q_PROPERTY(bool m3u8sWebDavPickerLoading READ m3u8sWebDavPickerLoading NOTIFY m3u8sWebDavPickerChanged)
    Q_PROPERTY(bool m3u8sUploading READ m3u8sUploading NOTIFY m3u8sPackagingChanged)
    Q_PROPERTY(double m3u8sUploadProgress READ m3u8sUploadProgress NOTIFY m3u8sPackagingChanged)
    Q_PROPERTY(QString m3u8sVideoEncoding READ m3u8sVideoEncoding WRITE setM3u8sVideoEncoding NOTIFY m3u8sSettingsChanged)
    Q_PROPERTY(QString m3u8sAudioEncoding READ m3u8sAudioEncoding WRITE setM3u8sAudioEncoding NOTIFY m3u8sSettingsChanged)
    Q_PROPERTY(QString m3u8sVideoQuality READ m3u8sVideoQuality WRITE setM3u8sVideoQuality NOTIFY m3u8sSettingsChanged)
    Q_PROPERTY(QString m3u8sContainerFormat READ m3u8sContainerFormat WRITE setM3u8sContainerFormat NOTIFY m3u8sSettingsChanged)
    Q_PROPERTY(bool webDavAudioPlaybackActive READ webDavAudioPlaybackActive NOTIFY webDavAudioPlaybackChanged)
    Q_PROPERTY(int webDavAudioCurrentIndex READ webDavAudioCurrentIndex NOTIFY webDavAudioPlaybackChanged)
    Q_PROPERTY(int webDavAudioQueueCount READ webDavAudioQueueCount NOTIFY webDavAudioPlaybackChanged)
    Q_PROPERTY(QString webDavAudioCurrentName READ webDavAudioCurrentName NOTIFY webDavAudioPlaybackChanged)
    Q_PROPERTY(QString webDavAudioRepeatMode READ webDavAudioRepeatMode WRITE setWebDavAudioRepeatMode NOTIFY webDavAudioRepeatModeChanged)
    Q_PROPERTY(QString defaultDownloadDirectory READ defaultDownloadDirectory WRITE setDefaultDownloadDirectory NOTIFY defaultDownloadDirectoryChanged)
    Q_PROPERTY(TransferTaskListModel* transferTasks READ transferTasks CONSTANT)
    Q_PROPERTY(TransferTaskListModel* transferDetailTasks READ transferDetailTasks CONSTANT)
    Q_PROPERTY(QString transferDetailFilter READ transferDetailFilter WRITE setTransferDetailFilter NOTIFY transferDetailFilterChanged)
    Q_PROPERTY(QString selectedTransferGroupId READ selectedTransferGroupId NOTIFY transferSelectionChanged)
    Q_PROPERTY(QString selectedTransferGroupTitle READ selectedTransferGroupTitle NOTIFY transferSelectionChanged)
    Q_PROPERTY(int activeTransferCount READ activeTransferCount NOTIFY transferTasksChanged)
    Q_PROPERTY(int completedTransferCount READ completedTransferCount NOTIFY transferTasksChanged)
    Q_PROPERTY(int failedTransferCount READ failedTransferCount NOTIFY transferTasksChanged)
    Q_PROPERTY(qint64 transferBytesPerSecond READ transferBytesPerSecond NOTIFY transferTasksChanged)
    Q_PROPERTY(qint64 transferAverageBytesPerSecond READ transferAverageBytesPerSecond NOTIFY transferTasksChanged)
    Q_PROPERTY(qint64 transferDownloadBytesPerSecond READ transferDownloadBytesPerSecond NOTIFY transferTasksChanged)
    Q_PROPERTY(qint64 transferUploadBytesPerSecond READ transferUploadBytesPerSecond NOTIFY transferTasksChanged)
    Q_PROPERTY(qint64 transferAverageDownloadBytesPerSecond READ transferAverageDownloadBytesPerSecond NOTIFY transferTasksChanged)
    Q_PROPERTY(qint64 transferAverageUploadBytesPerSecond READ transferAverageUploadBytesPerSecond NOTIFY transferTasksChanged)
    Q_PROPERTY(qint64 transferRemainingBytes READ transferRemainingBytes NOTIFY transferTasksChanged)
    Q_PROPERTY(QString playbackHttpUsername READ playbackHttpUsername NOTIFY playbackChanged)
    Q_PROPERTY(QString playbackHttpPassword READ playbackHttpPassword NOTIFY playbackChanged)
    Q_PROPERTY(bool playbackAllowInsecureTls READ playbackAllowInsecureTls NOTIFY playbackChanged)
    Q_PROPERTY(bool editingServices READ editingServices WRITE setEditingServices NOTIFY editingServicesChanged)
    Q_PROPERTY(bool minimizeToTray READ minimizeToTray WRITE setMinimizeToTray NOTIFY minimizeToTrayChanged)
    Q_PROPERTY(QString themeMode READ themeMode WRITE setThemeMode NOTIFY themeModeChanged)
    Q_PROPERTY(QString effectiveTheme READ effectiveTheme NOTIFY effectiveThemeChanged)
    Q_PROPERTY(QString languageMode READ languageMode WRITE setLanguageMode NOTIFY languageModeChanged)
    Q_PROPERTY(QString embyHomeLayout READ embyHomeLayout WRITE setEmbyHomeLayout NOTIFY embyHomeLayoutChanged)
    Q_PROPERTY(QVariantList embyRecommendationGenreOptions READ embyRecommendationGenreOptions NOTIFY embyRecommendationSettingsChanged)
    Q_PROPERTY(bool embyRecommendationGenresLoading READ embyRecommendationGenresLoading NOTIFY embyRecommendationSettingsChanged)
    Q_PROPERTY(bool embyRecommendationRefreshing READ embyRecommendationRefreshing NOTIFY embyRecommendationSettingsChanged)
    Q_PROPERTY(QString embyRecommendationRefreshStatus READ embyRecommendationRefreshStatus NOTIFY embyRecommendationSettingsChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString updateChannel READ updateChannel WRITE setUpdateChannel NOTIFY updateSettingsChanged)
    Q_PROPERTY(bool automaticUpdateCheck READ automaticUpdateCheck WRITE setAutomaticUpdateCheck NOTIFY updateSettingsChanged)
    Q_PROPERTY(bool updateChecking READ updateChecking NOTIFY updateStateChanged)
    Q_PROPERTY(bool updateDownloading READ updateDownloading NOTIFY updateStateChanged)
    Q_PROPERTY(double updateDownloadProgress READ updateDownloadProgress NOTIFY updateStateChanged)
    Q_PROPERTY(bool updateAvailable READ updateAvailable NOTIFY updateStateChanged)
    Q_PROPERTY(bool updateVersionValid READ updateVersionValid CONSTANT)
    Q_PROPERTY(QString latestUpdateVersion READ latestUpdateVersion NOTIFY updateStateChanged)
    Q_PROPERTY(QString latestUpdateNotes READ latestUpdateNotes NOTIFY updateStateChanged)
    Q_PROPERTY(QString latestUpdatePublishedAt READ latestUpdatePublishedAt NOTIFY updateStateChanged)
    Q_PROPERTY(QString updateStatus READ updateStatus NOTIFY updateStateChanged)
    Q_PROPERTY(QString updateLastCheckedAt READ updateLastCheckedAt NOTIFY updateStateChanged)
    Q_PROPERTY(QVariantList updateAssets READ updateAssets NOTIFY updateStateChanged)
    Q_PROPERTY(QString jellyfinHomeLayout READ jellyfinHomeLayout WRITE setJellyfinHomeLayout NOTIFY jellyfinHomeLayoutChanged)
    Q_PROPERTY(QString playerLayout READ playerLayout WRITE setPlayerLayout NOTIFY playerLayoutChanged)
    Q_PROPERTY(bool pageTransitionsEnabled READ pageTransitionsEnabled WRITE setPageTransitionsEnabled NOTIFY pageTransitionsEnabledChanged)
    Q_PROPERTY(int translationRevision READ translationRevision NOTIFY translationsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString loadingServiceCardId READ loadingServiceCardId NOTIFY loadingChanged)
    Q_PROPERTY(bool episodeSwitching READ episodeSwitching NOTIFY episodeSwitchingChanged)
    Q_PROPERTY(bool homeLoading READ homeLoading NOTIFY homeLoadingChanged)
    Q_PROPERTY(bool libraryItemsLoading READ libraryItemsLoading NOTIFY libraryItemsLoadingChanged)
    Q_PROPERTY(QString serverSearchText READ serverSearchText WRITE setServerSearchText NOTIFY serverSearchChanged)
    Q_PROPERTY(QString activeServerSearchTerm READ activeServerSearchTerm NOTIFY serverSearchChanged)
    Q_PROPERTY(bool serverSearchAvailable READ serverSearchAvailable NOTIFY currentServerChanged)
    Q_PROPERTY(bool serverSearchLoading READ serverSearchLoading NOTIFY serverSearchChanged)
    Q_PROPERTY(bool serverSearchHasMore READ serverSearchHasMore NOTIFY serverSearchChanged)
    Q_PROPERTY(bool loggedIn READ loggedIn NOTIFY loggedInChanged)
    Q_PROPERTY(QString currentUser READ currentUser NOTIFY currentUserChanged)
    Q_PROPERTY(QString currentServerName READ currentServerName NOTIFY currentServerChanged)
    Q_PROPERTY(QString currentLibraryName READ currentLibraryName NOTIFY currentLibraryChanged)
    Q_PROPERTY(QString currentView READ currentView NOTIFY currentViewChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorMessageChanged)
    Q_PROPERTY(bool hasError READ hasError NOTIFY errorMessageChanged)
    Q_PROPERTY(QString errorTitle READ errorTitle NOTIFY errorMessageChanged)
    Q_PROPERTY(QString errorSummary READ errorSummary NOTIFY errorMessageChanged)
    Q_PROPERTY(QString errorHint READ errorHint NOTIFY errorMessageChanged)
    Q_PROPERTY(QString errorDetails READ errorDetails NOTIFY errorMessageChanged)
    Q_PROPERTY(bool errorDetailsAvailable READ errorDetailsAvailable NOTIFY errorMessageChanged)
    Q_PROPERTY(QString selectedItemId READ selectedItemId NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemName READ selectedItemName NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemType READ selectedItemType NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemOverview READ selectedItemOverview NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemImageUrl READ selectedItemImageUrl NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemLogoUrl READ selectedItemLogoUrl NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemBackdropUrl READ selectedItemBackdropUrl NOTIFY selectedItemChanged)
    Q_PROPERTY(QStringList selectedItemBackdropUrls READ selectedItemBackdropUrls NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemMeta READ selectedItemMeta NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemSeasonEpisode READ selectedItemSeasonEpisode NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemPeople READ selectedItemPeople NOTIFY selectedItemChanged)
    Q_PROPERTY(PersonListModel* selectedItemPeopleModel READ selectedItemPeopleModel CONSTANT)
    Q_PROPERTY(double selectedItemPlayedPercentage READ selectedItemPlayedPercentage NOTIFY selectedItemChanged)
    Q_PROPERTY(bool selectedItemIsSeries READ selectedItemIsSeries NOTIFY selectedItemChanged)
    Q_PROPERTY(bool selectedItemHasSeriesEpisodes READ selectedItemHasSeriesEpisodes NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedSeasonId READ selectedSeasonId NOTIFY selectedSeasonChanged)
    Q_PROPERTY(QString selectedSeasonName READ selectedSeasonName NOTIFY selectedSeasonChanged)
    Q_PROPERTY(QUrl currentPlaybackUrl READ currentPlaybackUrl NOTIFY playbackChanged)
    Q_PROPERTY(double currentPlaybackStartSeconds READ currentPlaybackStartSeconds NOTIFY playbackChanged)
    Q_PROPERTY(int currentPlaybackSubtitleStreamIndex READ currentPlaybackSubtitleStreamIndex NOTIFY playbackChanged)
    Q_PROPERTY(ServiceCardListModel* services READ services CONSTANT)
    Q_PROPERTY(ServiceCardListModel* privacyCards READ privacyCards CONSTANT)
    Q_PROPERTY(bool privacyMode READ privacyMode NOTIFY privacyModeChanged)
    Q_PROPERTY(bool privacyPinConfigured READ privacyPinConfigured NOTIFY privacyPinChanged)
    Q_PROPERTY(MediaLibraryListModel* libraries READ libraries CONSTANT)
    Q_PROPERTY(MediaItemListModel* continueItems READ continueItems CONSTANT)
    Q_PROPERTY(MediaItemListModel* recommendedItems READ recommendedItems CONSTANT)
    Q_PROPERTY(MediaItemListModel* items READ items CONSTANT)
    Q_PROPERTY(MediaItemListModel* serverSearchResults READ serverSearchResults CONSTANT)
    Q_PROPERTY(MediaItemListModel* seriesSeasons READ seriesSeasons CONSTANT)
    Q_PROPERTY(MediaItemListModel* seriesEpisodes READ seriesEpisodes CONSTANT)
    Q_PROPERTY(DailyUsageStatsListModel* usageStats READ usageStats CONSTANT)
    Q_PROPERTY(PlaybackHistoryListModel* globalPlaybackHistory READ globalPlaybackHistory CONSTANT)
    Q_PROPERTY(QString globalHistoryFilter READ globalHistoryFilter WRITE setGlobalHistoryFilter NOTIFY globalHistoryFilterChanged)
    Q_PROPERTY(bool globalHistoryHasMore READ globalHistoryHasMore NOTIFY globalHistoryStateChanged)
    Q_PROPERTY(bool globalHistoryLoading READ globalHistoryLoading NOTIFY globalHistoryStateChanged)
    Q_PROPERTY(QStringList globalHistoryDates READ globalHistoryDates NOTIFY globalHistoryManagementChanged)
    Q_PROPERTY(QString globalHistoryManagementDate READ globalHistoryManagementDate NOTIFY globalHistoryManagementChanged)
    Q_PROPERTY(PlaybackHistoryListModel* globalHistoryDayItems READ globalHistoryDayItems CONSTANT)
    Q_PROPERTY(bool globalHistoryManagementLoading READ globalHistoryManagementLoading NOTIFY globalHistoryManagementChanged)
    Q_PROPERTY(qint64 historyTotalWatchSeconds READ historyTotalWatchSeconds NOTIFY historyStatsChanged)
    Q_PROPERTY(qint64 historyTotalNetworkBytes READ historyTotalNetworkBytes NOTIFY historyStatsChanged)
    Q_PROPERTY(qint64 historyTotalNetworkBytesIn READ historyTotalNetworkBytesIn NOTIFY historyStatsChanged)
    Q_PROPERTY(qint64 historyTotalNetworkBytesOut READ historyTotalNetworkBytesOut NOTIFY historyStatsChanged)
    Q_PROPERTY(qint64 historyNormalNetworkBytes READ historyNormalNetworkBytes NOTIFY historyStatsChanged)
    Q_PROPERTY(qint64 historyNormalNetworkBytesIn READ historyNormalNetworkBytesIn NOTIFY historyStatsChanged)
    Q_PROPERTY(qint64 historyNormalNetworkBytesOut READ historyNormalNetworkBytesOut NOTIFY historyStatsChanged)
    Q_PROPERTY(qint64 historyKeepAliveNetworkBytes READ historyKeepAliveNetworkBytes NOTIFY historyStatsChanged)
    Q_PROPERTY(qint64 historyKeepAliveNetworkBytesIn READ historyKeepAliveNetworkBytesIn NOTIFY historyStatsChanged)
    Q_PROPERTY(qint64 historyKeepAliveNetworkBytesOut READ historyKeepAliveNetworkBytesOut NOTIFY historyStatsChanged)
    Q_PROPERTY(int historyRetentionDays READ historyRetentionDays WRITE setHistoryRetentionDays NOTIFY historyRetentionChanged)
    Q_PROPERTY(ScheduledPlaybackTaskListModel* scheduledPlaybackTasks READ scheduledPlaybackTasks CONSTANT)
    Q_PROPERTY(ServiceCardListModel* scheduledEmbySources READ scheduledEmbySources CONSTANT)
    Q_PROPERTY(QString scheduledPlaybackStatus READ scheduledPlaybackStatus NOTIFY scheduledPlaybackStatusChanged)
    Q_PROPERTY(QString scheduledPlaybackServerName READ scheduledPlaybackServerName NOTIFY scheduledPlaybackStatusChanged)
    Q_PROPERTY(QString scheduledPlaybackMediaName READ scheduledPlaybackMediaName NOTIFY scheduledPlaybackStatusChanged)
    Q_PROPERTY(QString scheduledPlaybackError READ scheduledPlaybackError NOTIFY scheduledPlaybackStatusChanged)
    Q_PROPERTY(qint64 scheduledPlaybackElapsedSeconds READ scheduledPlaybackElapsedSeconds NOTIFY scheduledPlaybackStatusChanged)
    Q_PROPERTY(qint64 scheduledPlaybackTargetSeconds READ scheduledPlaybackTargetSeconds NOTIFY scheduledPlaybackStatusChanged)
    Q_PROPERTY(bool scheduledPlaybackActive READ scheduledPlaybackActive NOTIFY scheduledPlaybackStatusChanged)
    Q_PROPERTY(bool scheduledPlaybackWaiting READ scheduledPlaybackWaiting NOTIFY scheduledPlaybackStatusChanged)
    Q_PROPERTY(bool missedScheduledPlaybackPromptVisible READ missedScheduledPlaybackPromptVisible NOTIFY missedScheduledPlaybackTasksChanged)
    Q_PROPERTY(int missedScheduledPlaybackTaskCount READ missedScheduledPlaybackTaskCount NOTIFY missedScheduledPlaybackTasksChanged)
    Q_PROPERTY(QString missedScheduledPlaybackMessage READ missedScheduledPlaybackMessage NOTIFY missedScheduledPlaybackTasksChanged)
    Q_PROPERTY(int scheduledTaskSourceIndex READ scheduledTaskSourceIndex WRITE setScheduledTaskSourceIndex NOTIFY scheduledTaskEditorChanged)
    Q_PROPERTY(int scheduledTaskDurationMinutes READ scheduledTaskDurationMinutes WRITE setScheduledTaskDurationMinutes NOTIFY scheduledTaskEditorChanged)
    Q_PROPERTY(QString scheduledTaskScheduleType READ scheduledTaskScheduleType WRITE setScheduledTaskScheduleType NOTIFY scheduledTaskEditorChanged)
    Q_PROPERTY(int scheduledTaskStartHour READ scheduledTaskStartHour WRITE setScheduledTaskStartHour NOTIFY scheduledTaskEditorChanged)
    Q_PROPERTY(int scheduledTaskStartMinute READ scheduledTaskStartMinute WRITE setScheduledTaskStartMinute NOTIFY scheduledTaskEditorChanged)
    Q_PROPERTY(int scheduledTaskWeekday READ scheduledTaskWeekday WRITE setScheduledTaskWeekday NOTIFY scheduledTaskEditorChanged)
    Q_PROPERTY(int scheduledTaskMonthDay READ scheduledTaskMonthDay WRITE setScheduledTaskMonthDay NOTIFY scheduledTaskEditorChanged)
    Q_PROPERTY(QVariantList scheduledTaskCustomMonthDays READ scheduledTaskCustomMonthDays NOTIFY scheduledTaskEditorChanged)
    Q_PROPERTY(bool scheduledTaskEnabled READ scheduledTaskEnabled WRITE setScheduledTaskEnabled NOTIFY scheduledTaskEditorChanged)

public:
    explicit AppViewModel(QObject* parent = nullptr);

    QString serverUrl() const;
    void setServerUrl(const QString& value);

    QString serverName() const;
    void setServerName(const QString& value);

    QString username() const;
    void setUsername(const QString& value);

    QString password() const;
    void setPassword(const QString& value);

    QString serviceType() const;
    void setServiceType(const QString& value);

    bool trustSelfSignedCertificate() const;
    void setTrustSelfSignedCertificate(bool value);

    bool autoLogin() const;
    void setAutoLogin(bool value);

    QString iptvFilePath() const;
    void setIptvFilePath(const QString& value);
    QString iptvSearchText() const;
    void setIptvSearchText(const QString& value);
    QString iptvSelectedGroup() const;
    QStringList iptvGroups() const;
    IptvChannelListModel* iptvChannels();
    bool iptvPlaybackActive() const;
    QString currentIptvChannelId() const;
    LocalMediaRootListModel* localMediaRoots();
    LocalMediaItemListModel* localMediaItems();
    QString localMediaCurrentPath() const;
    QString localMediaRootName() const;
    bool localMediaDirectoryOpen() const;
    bool localMediaLoading() const;
    QString linkPlaybackAddress() const;
    void setLinkPlaybackAddress(const QString& value);
    LinkPlaybackHistoryListModel* linkPlaybackHistory();
    WebDavItemListModel* webDavItems();
    QString webDavCurrentPath() const;
    QString webDavDisplayMode() const;
    bool webDavShowM3u8sIdentifier() const;
    void setWebDavShowM3u8sIdentifier(bool enabled);
    bool webDavShowM3u8sSourceFileName() const;
    void setWebDavShowM3u8sSourceFileName(bool enabled);
    QString webDavTsslStatus() const;
    QString tsslBackupTarget() const;
    void setTsslBackupTarget(const QString& value);
    QString tsslBackupWebDavServiceId() const;
    void setTsslBackupWebDavServiceId(const QString& value);
    QString tsslBackupWebDavPath() const;
    void setTsslBackupWebDavPath(const QString& value);
    QVariantList tsslBackupWebDavServices() const;
    QString tsslBackupS3Endpoint() const;
    void setTsslBackupS3Endpoint(const QString& value);
    QString tsslBackupS3Bucket() const;
    void setTsslBackupS3Bucket(const QString& value);
    QString tsslBackupS3Region() const;
    void setTsslBackupS3Region(const QString& value);
    QString tsslBackupS3Prefix() const;
    void setTsslBackupS3Prefix(const QString& value);
    QString tsslBackupS3AccessKey() const;
    void setTsslBackupS3AccessKey(const QString& value);
    bool tsslBackupS3SecretConfigured() const;
    bool tsslBackupRunning() const;
    QString tsslBackupStatus() const;
    TsslPackageListModel* tsslPackages();
    TsslPackageListModel* tsslBatchPackages();
    bool m3u8sPackaging() const;
    double m3u8sPackagingProgress() const;
    QString m3u8sPackagingPhase() const;
    QString m3u8sStatus() const;
    bool m3u8sBatchExporting() const;
    QString m3u8sLastOutputDirectory() const;
    QStringList m3u8sSelectedSources() const;
    bool m3u8sFfmpegAvailable() const;
    int m3u8sSegmentDuration() const;
    void setM3u8sSegmentDuration(int value);
    QString m3u8sOutputDirectory() const;
    QString m3u8sOutputMode() const;
    void setM3u8sOutputMode(const QString& value);
    QString m3u8sWebDavServiceId() const;
    void setM3u8sWebDavServiceId(const QString& value);
    QString m3u8sWebDavPath() const;
    QString m3u8sFallbackDirectory() const;
    bool m3u8sKeepSuccessfulLocal() const;
    void setM3u8sKeepSuccessfulLocal(bool value);
    QVariantList m3u8sWebDavServices() const;
    WebDavItemListModel* m3u8sWebDavDirectories();
    QString m3u8sWebDavPickerPath() const;
    bool m3u8sWebDavPickerLoading() const;
    bool m3u8sUploading() const;
    double m3u8sUploadProgress() const;
    QString m3u8sVideoEncoding() const;
    void setM3u8sVideoEncoding(const QString& value);
    QString m3u8sAudioEncoding() const;
    void setM3u8sAudioEncoding(const QString& value);
    QString m3u8sVideoQuality() const;
    QString m3u8sContainerFormat() const;
    void setM3u8sContainerFormat(const QString& format);
    void setM3u8sVideoQuality(const QString& value);
    void setWebDavDisplayMode(const QString& value);
    bool webDavAudioPlaybackActive() const;
    int webDavAudioCurrentIndex() const;
    int webDavAudioQueueCount() const;
    QString webDavAudioCurrentName() const;
    QString webDavAudioRepeatMode() const;
    void setWebDavAudioRepeatMode(const QString& value);
    QString defaultDownloadDirectory() const;
    void setDefaultDownloadDirectory(const QString& value);
    TransferTaskListModel* transferTasks();
    TransferTaskListModel* transferDetailTasks();
    QString transferDetailFilter() const;
    void setTransferDetailFilter(const QString& value);
    QString selectedTransferGroupId() const;
    QString selectedTransferGroupTitle() const;
    int activeTransferCount() const;
    int completedTransferCount() const;
    int failedTransferCount() const;
    qint64 transferBytesPerSecond() const;
    qint64 transferAverageBytesPerSecond() const;
    qint64 transferDownloadBytesPerSecond() const;
    qint64 transferUploadBytesPerSecond() const;
    qint64 transferAverageDownloadBytesPerSecond() const;
    qint64 transferAverageUploadBytesPerSecond() const;
    qint64 transferRemainingBytes() const;
    QString playbackHttpUsername() const;
    QString playbackHttpPassword() const;
    bool playbackAllowInsecureTls() const;

    bool editingServices() const;
    void setEditingServices(bool value);

    bool minimizeToTray() const;
    void setMinimizeToTray(bool value);

    QString themeMode() const;
    void setThemeMode(const QString& value);
    QString effectiveTheme() const;
    QString languageMode() const;
    void setLanguageMode(const QString& value);
    QString embyHomeLayout() const;
    void setEmbyHomeLayout(const QString& value);
    QVariantList embyRecommendationGenreOptions() const;
    bool embyRecommendationGenresLoading() const;
    bool embyRecommendationRefreshing() const;
    QString embyRecommendationRefreshStatus() const;
    QString currentVersion() const;
    QString updateChannel() const;
    void setUpdateChannel(const QString& value);
    bool automaticUpdateCheck() const;
    void setAutomaticUpdateCheck(bool value);
    bool updateChecking() const;
    bool updateDownloading() const;
    double updateDownloadProgress() const;
    bool updateAvailable() const;
    bool updateVersionValid() const;
    QString latestUpdateVersion() const;
    QString latestUpdateNotes() const;
    QString latestUpdatePublishedAt() const;
    QString updateStatus() const;
    QString updateLastCheckedAt() const;
    QVariantList updateAssets() const;
    QString jellyfinHomeLayout() const;
    void setJellyfinHomeLayout(const QString& value);
    QString playerLayout() const;
    void setPlayerLayout(const QString& value);
    bool pageTransitionsEnabled() const;
    void setPageTransitionsEnabled(bool value);
    int translationRevision() const;

    bool loading() const;
    QString loadingServiceCardId() const;
    bool episodeSwitching() const;
    bool homeLoading() const;
    bool libraryItemsLoading() const;
    QString serverSearchText() const;
    void setServerSearchText(const QString& value);
    QString activeServerSearchTerm() const;
    bool serverSearchAvailable() const;
    bool serverSearchLoading() const;
    bool serverSearchHasMore() const;
    bool loggedIn() const;
    QString currentUser() const;
    QString currentServerName() const;
    QString currentLibraryName() const;
    QString currentView() const;
    QString errorMessage() const;
    bool hasError() const;
    QString errorTitle() const;
    QString errorSummary() const;
    QString errorHint() const;
    QString errorDetails() const;
    bool errorDetailsAvailable() const;
    QString selectedItemId() const;
    QString selectedItemName() const;
    QString selectedItemType() const;
    QString selectedItemOverview() const;
    QString selectedItemImageUrl() const;
    QString selectedItemLogoUrl() const;
    QString selectedItemBackdropUrl() const;
    QStringList selectedItemBackdropUrls() const;
    QString selectedItemMeta() const;
    QString selectedItemSeasonEpisode() const;
    QString selectedItemPeople() const;
    PersonListModel* selectedItemPeopleModel();
    double selectedItemPlayedPercentage() const;
    bool selectedItemIsSeries() const;
    bool selectedItemHasSeriesEpisodes() const;
    QString selectedSeasonId() const;
    QString selectedSeasonName() const;
    QUrl currentPlaybackUrl() const;
    double currentPlaybackStartSeconds() const;
    int currentPlaybackSubtitleStreamIndex() const;

    ServiceCardListModel* services();
    ServiceCardListModel* privacyCards();
    bool privacyMode() const;
    bool privacyPinConfigured() const;
    MediaLibraryListModel* libraries();
    MediaItemListModel* continueItems();
    MediaItemListModel* recommendedItems();
    MediaItemListModel* items();
    MediaItemListModel* serverSearchResults();
    MediaItemListModel* seriesSeasons();
    MediaItemListModel* seriesEpisodes();
    DailyUsageStatsListModel* usageStats();
    PlaybackHistoryListModel* globalPlaybackHistory();
    QString globalHistoryFilter() const;
    void setGlobalHistoryFilter(const QString& value);
    bool globalHistoryHasMore() const;
    bool globalHistoryLoading() const;
    QStringList globalHistoryDates() const;
    QString globalHistoryManagementDate() const;
    PlaybackHistoryListModel* globalHistoryDayItems();
    bool globalHistoryManagementLoading() const;
    qint64 historyTotalWatchSeconds() const;
    qint64 historyTotalNetworkBytes() const;
    qint64 historyTotalNetworkBytesIn() const;
    qint64 historyTotalNetworkBytesOut() const;
    qint64 historyNormalNetworkBytes() const;
    qint64 historyNormalNetworkBytesIn() const;
    qint64 historyNormalNetworkBytesOut() const;
    qint64 historyKeepAliveNetworkBytes() const;
    qint64 historyKeepAliveNetworkBytesIn() const;
    qint64 historyKeepAliveNetworkBytesOut() const;
    int historyRetentionDays() const;
    void setHistoryRetentionDays(int value);
    ScheduledPlaybackTaskListModel* scheduledPlaybackTasks();
    ServiceCardListModel* scheduledEmbySources();
    QString scheduledPlaybackStatus() const;
    QString scheduledPlaybackServerName() const;
    QString scheduledPlaybackMediaName() const;
    QString scheduledPlaybackError() const;
    qint64 scheduledPlaybackElapsedSeconds() const;
    qint64 scheduledPlaybackTargetSeconds() const;
    bool scheduledPlaybackActive() const;
    bool scheduledPlaybackWaiting() const;
    bool missedScheduledPlaybackPromptVisible() const;
    int missedScheduledPlaybackTaskCount() const;
    QString missedScheduledPlaybackMessage() const;
    int scheduledTaskSourceIndex() const;
    void setScheduledTaskSourceIndex(int value);
    int scheduledTaskDurationMinutes() const;
    void setScheduledTaskDurationMinutes(int value);
    QString scheduledTaskScheduleType() const;
    void setScheduledTaskScheduleType(const QString& value);
    int scheduledTaskStartHour() const;
    void setScheduledTaskStartHour(int value);
    int scheduledTaskStartMinute() const;
    void setScheduledTaskStartMinute(int value);
    int scheduledTaskWeekday() const;
    void setScheduledTaskWeekday(int value);
    int scheduledTaskMonthDay() const;
    void setScheduledTaskMonthDay(int value);
    QVariantList scheduledTaskCustomMonthDays() const;
    bool scheduledTaskEnabled() const;
    void setScheduledTaskEnabled(bool value);

    Q_INVOKABLE void initialize();
    Q_INVOKABLE void beginAddServiceCard();
    Q_INVOKABLE void login();
    Q_INVOKABLE void saveServiceCard();
    Q_INVOKABLE void selectServiceCard(int row);
    Q_INVOKABLE void editServiceCard(int row);
    Q_INVOKABLE void loginSelectedService(const QString& password);
    Q_INVOKABLE void chooseIptvPlaylistFile();
    Q_INVOKABLE void selectIptvGroup(const QString& groupName);
    Q_INVOKABLE void playIptvChannel(int row);
    Q_INVOKABLE void openLocalMedia();
    Q_INVOKABLE void addLocalMediaRoot(const QUrl& folderUrl);
    Q_INVOKABLE void openLocalMediaRoot(int row);
    Q_INVOKABLE void deleteLocalMediaRoot(int row);
    Q_INVOKABLE void openLocalMediaItem(int row);
    Q_INVOKABLE bool openDroppedLocalVideo(const QUrl& url, double replacedPositionSeconds);
    Q_INVOKABLE void localMediaBack();
    Q_INVOKABLE void refreshLocalMediaDirectory();
    Q_INVOKABLE void openLinkPlayback();
    Q_INVOKABLE bool playLink();
    Q_INVOKABLE bool playLinkHistory(const QString& recordId);
    Q_INVOKABLE void deleteLinkPlaybackHistory(const QString& recordId);
    Q_INVOKABLE void openWebDavItem(int row);
    Q_INVOKABLE void startWebDavAudioPlayback(int row = 0);
    Q_INVOKABLE void advanceWebDavAudioPlayback(bool reachedEnd, bool failed);
    Q_INVOKABLE void skipWebDavAudioTrack(int direction);
    Q_INVOKABLE void minimizeWebDavAudioPlayer();
    Q_INVOKABLE void restoreWebDavAudioPlayer();
    Q_INVOKABLE void webDavBack();
    Q_INVOKABLE void refreshWebDavDirectory();
    Q_INVOKABLE void chooseWebDavUploadFiles();
    Q_INVOKABLE void chooseWebDavUploadFolder();
    Q_INVOKABLE void downloadWebDavItem(int row);
    Q_INVOKABLE void restoreTssl();
    Q_INVOKABLE void exportWebDavTssl(int row);
    Q_INVOKABLE void openM3u8sManager();
    Q_INVOKABLE void refreshTsslPackages();
    Q_INVOKABLE void restoreManagedTssl();
    Q_INVOKABLE void exportManagedTssl(int row);
    Q_INVOKABLE void exportManagedTsslBatch(const QVariantList& rows);
    Q_INVOKABLE void deleteManagedTssl(int row);
    Q_INVOKABLE void deleteManagedTsslBatch(const QVariantList& rows);
    Q_INVOKABLE void addM3u8sVideoSource(const QUrl& file);
    Q_INVOKABLE void addM3u8sFolderSource(const QUrl& folder);
    Q_INVOKABLE void removeM3u8sSource(int index);
    Q_INVOKABLE void clearM3u8sSources();
    Q_INVOKABLE bool createM3u8sFromSelectedSources();
    Q_INVOKABLE void chooseM3u8sOutputDirectory();
    Q_INVOKABLE void chooseM3u8sFallbackDirectory();
    Q_INVOKABLE void chooseM3u8sWebDavDirectory();
    Q_INVOKABLE void selectM3u8sWebDavService(const QString& serviceId);
    Q_INVOKABLE void openM3u8sWebDavDirectory(int row);
    Q_INVOKABLE void m3u8sWebDavBack();
    Q_INVOKABLE void useCurrentM3u8sWebDavDirectory();
    Q_INVOKABLE void openM3u8sConfiguredOutputDirectory();
    Q_INVOKABLE void cancelM3u8sPackaging();
    Q_INVOKABLE void setTsslBackupS3Secret(const QString& secret);
    Q_INVOKABLE void backupTsslToConfiguredTarget();
    Q_INVOKABLE void cancelTsslBackup();
    Q_INVOKABLE void openM3u8sOutputDirectory();
    Q_INVOKABLE void openTsslStorageDirectory();
    Q_INVOKABLE void chooseDefaultDownloadDirectory();
    Q_INVOKABLE void openTransfers();
    Q_INVOKABLE void cancelTransfer(const QString& taskId);
    Q_INVOKABLE void pauseTransfer(const QString& taskId);
    Q_INVOKABLE void resumeTransfer(const QString& taskId);
    Q_INVOKABLE void retryTransfer(const QString& taskId);
    Q_INVOKABLE void clearFinishedTransfers();
    Q_INVOKABLE void openTransferGroup(const QString& groupId);
    Q_INVOKABLE void closeTransferGroup();
    Q_INVOKABLE bool unlockPrivacyMode(const QString& pin);
    Q_INVOKABLE void exitPrivacyMode();
    Q_INVOKABLE void refreshPrivacyCards();
    Q_INVOKABLE void setPrivacyCardPrivate(int row, bool privateMode);
    Q_INVOKABLE bool changePrivacyPin(const QString& oldPin, const QString& newPin, const QString& confirmPin);
    Q_INVOKABLE void recordPlaybackNetworkBytes(qint64 bytesReceived);
    Q_INVOKABLE void acceptPendingDownloadWarning(bool accepted);
    Q_INVOKABLE void startPendingFolderDownload();
    Q_INVOKABLE void deleteServiceCard(int row, bool deleteLocalData);
    Q_INVOKABLE void moveServiceCard(int row, int direction);
    Q_INVOKABLE void moveServiceCardTo(int fromRow, int toRow);
    Q_INVOKABLE QString trText(const QString& key) const;
    Q_INVOKABLE QString formatSeasonEpisode(const QString& season, const QString& episode) const;
    Q_INVOKABLE QString formatContinueProgress(double percentage) const;
    Q_INVOKABLE void logout();
    Q_INVOKABLE void backToServices();
    Q_INVOKABLE void backToHome();
    Q_INVOKABLE void mediaLibraryBack();
    Q_INVOKABLE void mediaDetailsBack();
    Q_INVOKABLE void openSettings();
    Q_INVOKABLE void openHistoryStats();
    Q_INVOKABLE void refreshHistoryStats();
    Q_INVOKABLE void openGlobalHistory();
    Q_INVOKABLE void refreshGlobalHistory();
    Q_INVOKABLE void loadMoreGlobalHistory();
    Q_INVOKABLE bool playGlobalHistory(const QString& recordId);
    Q_INVOKABLE void deleteGlobalHistory(const QString& recordId);
    Q_INVOKABLE void openGlobalHistoryManagement();
    Q_INVOKABLE void selectGlobalHistoryManagementDate(const QString& date);
    Q_INVOKABLE void deleteGlobalHistoryManagementDate();
    Q_INVOKABLE void cancelPendingHistoryReplay();
    Q_INVOKABLE void openScheduledPlaybackTasks();
    Q_INVOKABLE void beginAddScheduledPlaybackTask();
    Q_INVOKABLE void editScheduledPlaybackTask(int row);
    Q_INVOKABLE bool saveScheduledPlaybackTask();
    Q_INVOKABLE bool saveAndRunScheduledPlaybackTask();
    Q_INVOKABLE void toggleScheduledTaskCustomMonthDay(int day);
    Q_INVOKABLE void deleteScheduledPlaybackTask(int row);
    Q_INVOKABLE void runScheduledPlaybackTaskNow(int row);
    Q_INVOKABLE void stopScheduledPlayback();
    Q_INVOKABLE void resolveMissedScheduledPlaybackTasks(bool runNow);
    Q_INVOKABLE QString formatScheduledPlaybackSchedule(const QString& scheduleType,
                                                        const QString& startTime,
                                                        const QString& scheduleDays) const;
    Q_INVOKABLE QString formatDuration(qint64 seconds) const;
    Q_INVOKABLE void refreshHome();
    Q_INVOKABLE void refreshEmbyRecommendations();
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void downloadUpdate(const QString& assetName);
    Q_INVOKABLE void cancelUpdateDownload();
    Q_INVOKABLE void setEmbyRecommendationGenreExcluded(const QString& genre, bool excluded);
    Q_INVOKABLE void refreshLibraries();
    Q_INVOKABLE void searchMediaServer();
    Q_INVOKABLE void clearServerSearch();
    Q_INVOKABLE void loadMoreServerSearchResults();
    Q_INVOKABLE void openServerSearchItem(int row);
    Q_INVOKABLE void openLibrary(int row);
    Q_INVOKABLE void openContinueItem(int row);
    Q_INVOKABLE void openRecommendedItem(int row);
    Q_INVOKABLE void openItem(int row);
    Q_INVOKABLE void selectSeason(int row);
    Q_INVOKABLE void openEpisode(int row);
    Q_INVOKABLE void playSelectedItem();
    Q_INVOKABLE void reportPlaybackStarted();
    Q_INVOKABLE void reportPlaybackProgress(double positionSeconds, double durationSeconds, bool paused);
    Q_INVOKABLE void reportPlaybackStopped(double positionSeconds);
    Q_INVOKABLE void reportPlaybackEnded(double positionSeconds, bool reachedEnd, bool failed);
    Q_INVOKABLE void reportPlaybackError(const QString& message);
    Q_INVOKABLE void closePlayerToDetails();
    Q_INVOKABLE void loadMoreItems();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void copyCurrentErrorDetails();

    void openLocalPlaybackForVerification(const QUrl& url);

signals:
    void serverUrlChanged();
    void serverNameChanged();
    void usernameChanged();
    void passwordChanged();
    void serviceTypeChanged();
    void trustSelfSignedCertificateChanged();
    void autoLoginChanged();
    void iptvFilePathChanged();
    void iptvSearchTextChanged();
    void iptvSelectedGroupChanged();
    void iptvGroupsChanged();
    void localMediaDirectoryChanged();
    void localMediaLoadingChanged();
    void linkPlaybackAddressChanged();
    void webDavCurrentPathChanged();
    void webDavDisplayModeChanged();
    void webDavDisplaySettingsChanged();
    void webDavTsslStatusChanged();
    void tsslBackupChanged();
    void m3u8sPackagingChanged();
    void m3u8sStatusChanged();
    void m3u8sSourceSelectionChanged();
    void m3u8sSegmentDurationChanged();
    void m3u8sSettingsChanged();
    void m3u8sWebDavPickerChanged();
    void webDavAudioPlaybackChanged();
    void webDavAudioRepeatModeChanged();
    void defaultDownloadDirectoryChanged();
    void transferTasksChanged();
    void transferSelectionChanged();
    void transferDetailFilterChanged();
    void privacyModeChanged();
    void privacyPinChanged();
    void editingServicesChanged();
    void minimizeToTrayChanged();
    void themeModeChanged();
    void effectiveThemeChanged();
    void languageModeChanged();
    void embyHomeLayoutChanged();
    void embyRecommendationSettingsChanged();
    void updateSettingsChanged();
    void updateStateChanged();
    void jellyfinHomeLayoutChanged();
    void playerLayoutChanged();
    void pageTransitionsEnabledChanged();
    void translationsChanged();
    void loadingChanged();
    void episodeSwitchingChanged();
    void homeLoadingChanged();
    void libraryItemsLoadingChanged();
    void serverSearchChanged();
    void loggedInChanged();
    void currentUserChanged();
    void currentServerChanged();
    void currentLibraryChanged();
    void currentViewChanged();
    void errorMessageChanged();
    void selectedItemChanged();
    void selectedSeasonChanged();
    void playbackChanged();
    void historyStatsChanged();
    void historyRetentionChanged();
    void globalHistoryFilterChanged();
    void globalHistoryStateChanged();
    void globalHistoryManagementChanged();
    void scheduledPlaybackTasksChanged();
    void scheduledPlaybackStatusChanged();
    void missedScheduledPlaybackTasksChanged();
    void scheduledTaskEditorChanged();
    void passwordRequired(const QString& serviceName, const QString& username);
    void downloadSpaceWarningRequested(const QString& title, const QString& message);

private:
    enum class PlaybackOrigin {
        None,
        MediaServer,
        Iptv,
        WebDav,
        Local,
        Link,
    };

    struct PendingUsageStat {
        ServerConfig server;
        bool privacyMode { false };
        qint64 watchSeconds { 0 };
        qint64 networkBytesIn { 0 };
        qint64 networkBytesOut { 0 };
        qint64 keepAliveNetworkBytesIn { 0 };
        qint64 keepAliveNetworkBytesOut { 0 };
    };

    struct PlaybackProgressSnapshot {
        qint64 positionTicks { 0 };
        double playedPercentage { 0.0 };
        bool played { false };
        QDateTime reportedAt;
    };

    MediaServiceClient* clientFor(ServiceType type);
    ServerConfig makeServerConfig() const;
    void refreshServiceCards();
    void refreshLocalMediaRoots();
    void loadLocalMediaDirectory(const QString& path);
    void clearLocalMediaDirectory();
    bool localMediaPathIsInsideRoot(const QString& path) const;
    void startLocalVideoPlayback(const QString& path,
                                 const QString& displayName,
                                 bool retainLocalDirectory,
                                 double replacedPositionSeconds = -1.0,
                                 double startPositionSeconds = 0.0);
    void finishLocalVideoPlayback(const QString& path,
                                  const QUrl& playbackUrl,
                                  const QString& displayName,
                                  bool retainLocalDirectory,
                                  double replacedPositionSeconds,
                                  double startPositionSeconds,
                                  const QString& encryptedSessionId = {});
    void startLogin(const ServerConfig& server, const QString& password);
    void loadServiceHome();
    void loadIptvService(const ServiceCard& card);
    void applyIptvService(const ServiceCard& card,
                          IptvPlaylist playlist,
                          std::vector<IptvChannel> channels);
    void clearIptvState();
    void applyIptvFilters();
    std::expected<IptvPlaylist, QString> importIptvPlaylistFile(const ServerConfig& server, std::vector<IptvChannel>& channels) const;
    void loadWebDavService(const ServiceCard& card, const QString& password);
    void clearWebDavState();
    void loadWebDavDirectory(const QUrl& url);
    void rebuildWebDavAudioQueue(const std::vector<WebDavItem>& items);
    void playWebDavAudioTrack(int index);
    void startWebDavHistoryPlayback(const ServiceCard& card,
                                    const QString& password,
                                    const PlaybackHistoryItem& historyItem);
    void finishWebDavHistoryPlayback(const ServiceCard& card,
                                     const QString& password,
                                     const PlaybackHistoryItem& historyItem,
                                     const QUrl& remoteUrl,
                                     const QUrl& proxyUrl,
                                     const QString& encryptedSessionId = {});
    void startWebDavVideoPlayback(const WebDavItem& item,
                                  const QUrl& proxyUrl,
                                  const QString& encryptedSessionId = {});
    void clearWebDavAudioPlayback();
    void saveWebDavCredentials(const ServerConfig& server, const QString& password);
    std::optional<QString> loadWebDavPassword(const ServerConfig& server);
    QUrl childWebDavUrl(const QString& name, bool directory) const;
    void loadM3u8sWebDavDirectory(const QUrl& url);
    void enqueueM3u8sPackageUpload(const EncryptedHlsPackageResult& result);
    void finishM3u8sExportIfReady();
    QString uniqueLocalPath(const QString& directory, const QString& name) const;
    void enqueueWebDavUploadFile(const QString& localPath, const QUrl& remoteUrl);
    void wireUsageSignals();
    bool accumulateUsage(const ServerConfig& server,
                         bool privacyMode,
                         qint64 watchSeconds,
                         qint64 bytesReceived,
                         qint64 bytesSent,
                         NetworkTrafficCategory trafficCategory = NetworkTrafficCategory::Normal);
    void flushPendingUsageStats(bool refreshAfterFlush);
    void refreshUsageStats();
    void refreshLinkPlaybackHistory();
    void recordLinkPlaybackHistory(const QString& recordId, const QUrl& playbackUrl, const QDateTime& playedAt);
    bool startLinkPlayback(const QUrl& playbackUrl, double startPositionSeconds = 0.0);
    void startIptvChannelPlayback(const IptvChannel& channel);
    void loadGlobalHistoryPage(bool resetItems);
    void loadGlobalHistoryManagementDates();
    void loadGlobalHistoryManagementDate(const QString& date);
    std::vector<PlaybackHistoryItem> prepareGlobalHistoryItems(std::vector<PlaybackHistoryItem> items);
    PlaybackHistorySource selectedGlobalHistorySource() const;
    void recordGlobalPlaybackStarted();
    void updateGlobalPlaybackProgress(double positionSeconds, double durationSeconds, bool forceUpdate = false, bool completed = false);
    void finishGlobalPlaybackHistory(double positionSeconds, bool completed);
    bool replayMediaServerHistory(const PlaybackHistoryItem& historyItem);
    std::optional<ServiceCard> serviceCardForHistory(const QString& serviceId);
    bool webDavHistoryTargetIsValid(const ServerConfig& server, const QUrl& target) const;
    void refreshScheduledPlaybackTasks();
    void refreshScheduledEmbySources();
    std::optional<ScheduledPlaybackTask> scheduledPlaybackTaskFromEditor();
    bool saveScheduledPlaybackTaskInternal(bool runNow);
    void setForegroundPlaybackActive(bool active);
    void recordNetworkUsage(const ServerConfig& server, qint64 bytesReceived, qint64 bytesSent);
    void recordNetworkUsageForCurrentService(ServiceType type, qint64 bytesReceived, qint64 bytesSent);
    std::optional<ServerConfig> currentServerForUsage(ServiceType type) const;
    std::optional<ServerConfig> currentPlaybackServerForUsage() const;
    void beginPlaybackUsageTracking();
    void recordPlaybackUsageUntilNow();
    void finishPlaybackUsageTracking();
    void applyReportedPlaybackProgress(const QString& itemId, qint64 positionTicks);
    void mergeRecentPlaybackProgress(std::vector<MediaItem>& items) const;
    void refreshContinueWatching();
    void refreshRecommendations(bool force = false);
    void applyEmbyRecommendationFilter();
    bool mergeEmbyRecommendationGenres(const QStringList& genres);
    bool mergeEmbyRecommendationGenresFromItems();
    void refreshEmbyRecommendationGenres();
    void resetEmbyRecommendationRuntimeState();
    void clearServerSearchState(bool clearText = true);
    void loadServerSearchResults(bool resetItems);
    void openMediaItemDetails(const MediaItem& item, bool returnToSearch);
    void resetMediaDirectory(const QString& id, const QString& name);
    void clearMediaDirectoryState();
    void loadMediaDirectory(bool resetItems);
    bool isNavigableMediaFolder(const MediaItem& item) const;
    void clearSeriesDetails();
    void loadSeriesSeasons();
    void loadSeasonEpisodes(const MediaItem& season);
    void clearCurrentPlayback(double stopPositionSeconds = -1.0);
    void syncSelectedPeople();
    void setCurrentView(QString view);
    void setLoading(bool value);
    void setEpisodeSwitching(bool value);
    void beginHomeLoading();
    void endHomeLoading();
    void setLibraryItemsLoading(bool value);
    bool failInitialServiceLoad(const QString& message);
    void completeInitialServiceLoad();
    void setError(QString message);
    void setWebDavTsslStatus(QString message);
    void setSession(UserSession session);
    void saveSession();
    bool verifyPrivacyPin(const QString& pin) const;
    QString privacyPinHash(const QString& pin, const QString& salt) const;
    bool pinLooksValid(const QString& pin) const;

    QString m_serverUrl;
    QString m_serverName;
    QString m_username;
    QString m_password;
    QString m_iptvFilePath;
    QString m_iptvSearchText;
    QString m_iptvSelectedGroup;
    QStringList m_iptvGroups;
    QString m_linkPlaybackAddress;
    QString m_globalHistoryFilter { QStringLiteral("All") };
    QString m_webDavPassword;
    QString m_webDavDisplayMode { QStringLiteral("default") };
    bool m_webDavShowM3u8sIdentifier { true };
    bool m_webDavShowM3u8sSourceFileName { true };
    QString m_webDavTsslStatus;
    std::vector<WebDavItem> m_webDavAudioQueue;
    int m_webDavAudioCurrentIndex { -1 };
    bool m_webDavAudioPlaybackActive { false };
    QString m_webDavAudioRepeatMode { QStringLiteral("off") };
    QString m_defaultDownloadDirectory;
    QString m_transferDetailFilter { QStringLiteral("all") };
    ServiceType m_serviceType { ServiceType::Emby };
    bool m_trustSelfSignedCertificate { true };
    bool m_autoLogin { true };
    bool m_editingServices { false };
    bool m_privacyMode { false };
    QString m_themeMode { QStringLiteral("dark") };
    QString m_languageMode { QStringLiteral("system") };
    int m_translationRevision { 0 };
    bool m_loading { false };
    bool m_episodeSwitching { false };
    int m_homeLoadingRequests { 0 };
    bool m_initialServiceLoadActive { false };
    bool m_initialServiceHasValidData { false };
    QString m_initialServiceError;
    std::vector<MediaItem> m_unfilteredEmbyRecommendations;
    QDateTime m_embyRecommendationUpdatedAt;
    QString m_embyRecommendationStatus { QStringLiteral("idle") };
    bool m_embyRecommendationRefreshing { false };
    bool m_embyRecommendationGenresLoading { false };
    quint64 m_embyRecommendationGenreRequestId { 0 };
    bool m_libraryItemsLoading { false };
    QString m_serverSearchText;
    QString m_activeServerSearchTerm;
    bool m_serverSearchLoading { false };
    int m_serverSearchNextStartIndex { 0 };
    int m_serverSearchPageSize { 36 };
    bool m_serverSearchHasMore { false };
    int m_serverSearchRequestGeneration { 0 };
    bool m_detailsReturnToSearch { false };
    QString m_errorMessage;
    AppErrorPresentation m_errorPresentation;
    std::optional<UserSession> m_session;
    std::optional<ServiceCard> m_pendingServiceCard;
    std::optional<ServiceCard> m_currentIptvCard;
    std::optional<IptvPlaylist> m_currentIptvPlaylist;
    QString m_currentIptvChannelId;
    std::optional<LocalMediaRoot> m_currentLocalMediaRoot;
    QString m_localMediaCurrentPath;
    bool m_localMediaLoading { false };
    quint64 m_localMediaRequestGeneration { 0 };
    std::optional<ServiceCard> m_currentWebDavCard;
    QUrl m_webDavCurrentUrl;
    std::vector<QUrl> m_webDavHistory;
    std::vector<WebDavItem> m_webDavDirectoryItems;
    quint64 m_webDavDirectoryRequestGeneration { 0 };
    std::optional<MediaLibrary> m_currentLibrary;
    QString m_currentMediaParentId;
    QString m_currentMediaParentName;
    std::vector<std::pair<QString, QString>> m_mediaParentHistory;
    std::optional<MediaItem> m_selectedItem;
    std::optional<MediaItem> m_selectedSeason;
    QUrl m_currentPlaybackUrl;
    QString m_currentLocalPlaybackPath;
    QString m_currentMediaSourceId;
    QString m_currentPlaySessionId;
    int m_currentPlaybackSubtitleStreamIndex { -1 };
    QString m_playbackHttpUsername;
    QString m_playbackHttpPassword;
    bool m_playbackAllowInsecureTls { false };
    QString m_webDavPlaybackStreamId;
    QString m_encryptedHlsPlaybackSessionId;
    quint64 m_encryptedHlsPrepareGeneration { 0 };
    bool m_encryptedHlsPreparing { false };
    double m_currentPlaybackStartSeconds { 0.0 };
    double m_lastPlaybackReportSeconds { -1.0 };
    bool m_playbackStartedReported { false };
    QString m_currentPlaybackHistoryId;
    double m_currentPlaybackPositionSeconds { 0.0 };
    double m_currentPlaybackDurationSeconds { 0.0 };
    double m_lastHistoryPersistedPositionSeconds { -1.0 };
    QDateTime m_lastHistoryPersistedAt;
    PlaybackOrigin m_playbackOrigin { PlaybackOrigin::None };
    bool m_playbackUsageActive { false };
    bool m_playbackUsagePaused { false };
    std::optional<ServerConfig> m_playbackUsageServer;
    QDateTime m_playbackUsageLastWallClock;
    QString m_currentView { QStringLiteral("services") };
    int m_nextItemStartIndex { 0 };
    int m_itemPageSize { 80 };
    bool m_hasMoreMediaItems { true };
    int m_seriesRequestGeneration { 0 };
    int m_episodeDetailRequestGeneration { 0 };
    int m_globalHistoryReplayGeneration { 0 };
    int m_globalHistoryNextStartIndex { 0 };
    int m_globalHistoryPageSize { 60 };
    bool m_globalHistoryHasMore { false };
    bool m_globalHistoryLoading { false };
    QStringList m_globalHistoryDates;
    QString m_globalHistoryManagementDate;
    bool m_globalHistoryManagementLoading { false };
    std::optional<PlaybackHistoryItem> m_pendingHistoryReplay;
    QString m_scheduledTaskEditingId;
    int m_scheduledTaskSourceIndex { -1 };
    int m_scheduledTaskDurationMinutes { 90 };
    QString m_scheduledTaskScheduleType { QStringLiteral("manual") };
    int m_scheduledTaskStartHour { 12 };
    int m_scheduledTaskStartMinute { 0 };
    int m_scheduledTaskWeekday { 1 };
    int m_scheduledTaskMonthDay { 1 };
    QList<int> m_scheduledTaskCustomMonthDays { 1 };
    bool m_scheduledTaskEnabled { true };

    NetworkClient m_embyNetworkClient;
    NetworkClient m_jellyfinNetworkClient;
    EmbyClient m_embyClient;
    JellyfinClient m_jellyfinClient;
    WebDavClient m_webDavClient;
    TsslBackupService m_tsslBackupService;
    WebDavDownloadPlanner m_webDavDownloadPlanner;
    WebDavPlaybackProxy m_webDavPlaybackProxy;
    TsslStore m_tsslStore;
    EncryptedHlsPlaybackProxy m_encryptedHlsPlaybackProxy;
    EncryptedHlsBatchPackager m_m3u8sPackager;
    LocalMediaService m_localMediaService;
    TransferManager m_transferManager;
    SessionRepository m_repository;
    UpdateService m_updateService;
    ScheduledPlaybackManager m_scheduledPlaybackManager;
    ServiceCardListModel m_services;
    ServiceCardListModel m_privacyCards;
    ServiceCardListModel m_scheduledEmbySources;
    ScheduledPlaybackTaskListModel m_scheduledPlaybackTasks;
    MediaLibraryListModel m_libraries;
    MediaItemListModel m_continueItems;
    MediaItemListModel m_recommendedItems;
    MediaItemListModel m_items;
    MediaItemListModel m_serverSearchResults;
    MediaItemListModel m_seriesSeasons;
    MediaItemListModel m_seriesEpisodes;
    IptvChannelListModel m_iptvChannels;
    LocalMediaRootListModel m_localMediaRoots;
    LocalMediaItemListModel m_localMediaItems;
    WebDavItemListModel m_webDavItems;
    LinkPlaybackHistoryListModel m_linkPlaybackHistory;
    DailyUsageStatsListModel m_usageStats;
    PlaybackHistoryListModel m_globalPlaybackHistory;
    PlaybackHistoryListModel m_globalHistoryDayItems;
    TsslPackageListModel m_tsslPackages;
    TsslPackageListModel m_tsslBatchPackages;
    QString m_m3u8sStatus;
    bool m_m3u8sBatchExporting { false };
    QString m_m3u8sLastOutputDirectory;
    QStringList m_m3u8sSelectedSources;
    std::shared_ptr<std::atomic_bool> m_m3u8sSourceScanCanceled;
    bool m_m3u8sPreparing { false };
    int m_m3u8sSegmentDuration { 6 };
    QString m_m3u8sOutputDirectory;
    QString m_m3u8sOutputMode { QStringLiteral("local") };
    QString m_m3u8sWebDavServiceId;
    QString m_m3u8sWebDavPath;
    QString m_m3u8sFallbackDirectory;
    bool m_m3u8sKeepSuccessfulLocal { false };
    WebDavItemListModel m_m3u8sWebDavDirectories;
    std::optional<ServiceCard> m_m3u8sWebDavCard;
    QString m_m3u8sWebDavPassword;
    QUrl m_m3u8sWebDavPickerUrl;
    QList<QUrl> m_m3u8sWebDavPickerHistory;
    quint64 m_m3u8sWebDavPickerGeneration { 0 };
    bool m_m3u8sWebDavPickerLoading { false };
    std::unique_ptr<QTemporaryDir> m_m3u8sStagingDirectory;
    bool m_m3u8sUploading { false };
    bool m_m3u8sBatchCompleted { false };
    bool m_m3u8sCancelRequested { false };
    QHash<QString, QString> m_m3u8sUploadTaskPaths;
    QHash<QString, qint64> m_m3u8sUploadTaskDone;
    QHash<QString, qint64> m_m3u8sUploadTaskTotals;
    qint64 m_m3u8sUploadDoneBytes { 0 };
    qint64 m_m3u8sUploadTotalBytes { 0 };
    int m_m3u8sPendingUploads { 0 };
    int m_m3u8sUploadFailures { 0 };
    QString m_m3u8sVideoEncoding { QStringLiteral("h264") };
    QString m_m3u8sAudioEncoding { QStringLiteral("aac") };
    QString m_m3u8sVideoQuality { QStringLiteral("balanced") };
    QString m_m3u8sContainerFormat { QStringLiteral("m3u8sp") };
    QString m_tsslBackupTarget { QStringLiteral("none") };
    QString m_tsslBackupWebDavServiceId;
    QString m_tsslBackupWebDavPath { QStringLiteral("vibePlayerQT/tssl") };
    QString m_tsslBackupS3Endpoint;
    QString m_tsslBackupS3Bucket;
    QString m_tsslBackupS3Region { QStringLiteral("us-east-1") };
    QString m_tsslBackupS3Prefix { QStringLiteral("vibePlayerQT/tssl") };
    QString m_tsslBackupS3AccessKey;
    bool m_tsslBackupS3SecretConfigured { false };
    bool m_tsslBackupRunning { false };
    QString m_tsslBackupStatus;
    qint64 m_historyTotalWatchSeconds { 0 };
    qint64 m_historyTotalNetworkBytes { 0 };
    qint64 m_historyTotalNetworkBytesIn { 0 };
    qint64 m_historyTotalNetworkBytesOut { 0 };
    int m_historyRetentionDays { 30 };
    qint64 m_historyNormalNetworkBytes { 0 };
    qint64 m_historyNormalNetworkBytesIn { 0 };
    qint64 m_historyNormalNetworkBytesOut { 0 };
    qint64 m_historyKeepAliveNetworkBytes { 0 };
    qint64 m_historyKeepAliveNetworkBytesIn { 0 };
    qint64 m_historyKeepAliveNetworkBytesOut { 0 };
    QHash<QString, PendingUsageStat> m_pendingUsageStats;
    QHash<QString, PlaybackProgressSnapshot> m_recentPlaybackProgress;
    QTimer m_usageFlushTimer;
    std::vector<IptvChannel> m_allIptvChannels;
    PersonListModel m_selectedPeople;
    std::function<void(bool)> m_pendingDownloadWarningReply;
    std::function<void()> m_pendingFolderDownload;
    bool m_updateChecking { false };
    bool m_updateDownloading { false };
    double m_updateDownloadProgress { 0.0 };
    bool m_updateAvailable { false };
    bool m_updateVersionValid { true };
    QString m_latestUpdateVersion;
    QString m_latestUpdateNotes;
    QDateTime m_latestUpdatePublishedAt;
    QString m_updateStatus;
    QList<UpdateAsset> m_updateAssets;
};
