#pragma once

#include "database/SessionRepository.h"
#include "models/IptvChannel.h"
#include "models/IptvPlaylist.h"
#include "models/UserSession.h"
#include "network/NetworkClient.h"
#include "services/webdav/TransferManager.h"
#include "services/webdav/WebDavClient.h"
#include "services/webdav/WebDavDownloadPlanner.h"
#include "services/webdav/WebDavPlaybackProxy.h"
#include "services/emby/EmbyClient.h"
#include "services/jellyfin/JellyfinClient.h"
#include "services/scheduler/ScheduledPlaybackManager.h"
#include "viewmodels/IptvChannelListModel.h"
#include "viewmodels/DailyUsageStatsListModel.h"
#include "viewmodels/MediaItemListModel.h"
#include "viewmodels/MediaLibraryListModel.h"
#include "viewmodels/PersonListModel.h"
#include "viewmodels/ServiceCardListModel.h"
#include "viewmodels/ScheduledPlaybackTaskListModel.h"
#include "viewmodels/WebDavItemListModel.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariantList>

#include <functional>
#include <memory>
#include <optional>

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
    Q_PROPERTY(WebDavItemListModel* webDavItems READ webDavItems CONSTANT)
    Q_PROPERTY(QString webDavCurrentPath READ webDavCurrentPath NOTIFY webDavCurrentPathChanged)
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
    Q_PROPERTY(bool pageTransitionsEnabled READ pageTransitionsEnabled WRITE setPageTransitionsEnabled NOTIFY pageTransitionsEnabledChanged)
    Q_PROPERTY(int translationRevision READ translationRevision NOTIFY translationsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
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
    Q_PROPERTY(QString selectedItemName READ selectedItemName NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemType READ selectedItemType NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemOverview READ selectedItemOverview NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemImageUrl READ selectedItemImageUrl NOTIFY selectedItemChanged)
    Q_PROPERTY(QString selectedItemBackdropUrl READ selectedItemBackdropUrl NOTIFY selectedItemChanged)
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
    Q_PROPERTY(ServiceCardListModel* services READ services CONSTANT)
    Q_PROPERTY(ServiceCardListModel* privacyCards READ privacyCards CONSTANT)
    Q_PROPERTY(bool privacyMode READ privacyMode NOTIFY privacyModeChanged)
    Q_PROPERTY(bool privacyPinConfigured READ privacyPinConfigured NOTIFY privacyPinChanged)
    Q_PROPERTY(MediaLibraryListModel* libraries READ libraries CONSTANT)
    Q_PROPERTY(MediaItemListModel* continueItems READ continueItems CONSTANT)
    Q_PROPERTY(MediaItemListModel* items READ items CONSTANT)
    Q_PROPERTY(MediaItemListModel* serverSearchResults READ serverSearchResults CONSTANT)
    Q_PROPERTY(MediaItemListModel* seriesSeasons READ seriesSeasons CONSTANT)
    Q_PROPERTY(MediaItemListModel* seriesEpisodes READ seriesEpisodes CONSTANT)
    Q_PROPERTY(DailyUsageStatsListModel* usageStats READ usageStats CONSTANT)
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
    WebDavItemListModel* webDavItems();
    QString webDavCurrentPath() const;
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
    bool pageTransitionsEnabled() const;
    void setPageTransitionsEnabled(bool value);
    int translationRevision() const;

    bool loading() const;
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
    QString selectedItemName() const;
    QString selectedItemType() const;
    QString selectedItemOverview() const;
    QString selectedItemImageUrl() const;
    QString selectedItemBackdropUrl() const;
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

    ServiceCardListModel* services();
    ServiceCardListModel* privacyCards();
    bool privacyMode() const;
    bool privacyPinConfigured() const;
    MediaLibraryListModel* libraries();
    MediaItemListModel* continueItems();
    MediaItemListModel* items();
    MediaItemListModel* serverSearchResults();
    MediaItemListModel* seriesSeasons();
    MediaItemListModel* seriesEpisodes();
    DailyUsageStatsListModel* usageStats();
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
    Q_INVOKABLE void openWebDavItem(int row);
    Q_INVOKABLE void webDavBack();
    Q_INVOKABLE void refreshWebDavDirectory();
    Q_INVOKABLE void chooseWebDavUploadFiles();
    Q_INVOKABLE void chooseWebDavUploadFolder();
    Q_INVOKABLE void downloadWebDavItem(int row);
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
    Q_INVOKABLE void refreshLibraries();
    Q_INVOKABLE void searchMediaServer();
    Q_INVOKABLE void clearServerSearch();
    Q_INVOKABLE void loadMoreServerSearchResults();
    Q_INVOKABLE void openServerSearchItem(int row);
    Q_INVOKABLE void openLibrary(int row);
    Q_INVOKABLE void openContinueItem(int row);
    Q_INVOKABLE void openItem(int row);
    Q_INVOKABLE void selectSeason(int row);
    Q_INVOKABLE void openEpisode(int row);
    Q_INVOKABLE void playSelectedItem();
    Q_INVOKABLE void reportPlaybackStarted();
    Q_INVOKABLE void reportPlaybackProgress(double positionSeconds, bool paused);
    Q_INVOKABLE void reportPlaybackStopped(double positionSeconds);
    Q_INVOKABLE void reportPlaybackError(const QString& message);
    Q_INVOKABLE void closePlayerToDetails();
    Q_INVOKABLE void loadMoreItems();
    Q_INVOKABLE void clearError();
    Q_INVOKABLE void acceptPendingCertificate(bool accepted);

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
    void webDavCurrentPathChanged();
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
    void pageTransitionsEnabledChanged();
    void translationsChanged();
    void loadingChanged();
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
    void scheduledPlaybackTasksChanged();
    void scheduledPlaybackStatusChanged();
    void missedScheduledPlaybackTasksChanged();
    void scheduledTaskEditorChanged();
    void certificatePromptRequested(const QString& host, const QString& details);
    void passwordRequired(const QString& serviceName, const QString& username);
    void downloadSpaceWarningRequested(const QString& title, const QString& message);

private:
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
    void startLogin(const ServerConfig& server, const QString& password);
    void loadServiceHome();
    void loadIptvService(const ServiceCard& card);
    void clearIptvState();
    void refreshIptvChannels();
    void applyIptvFilters();
    std::expected<IptvPlaylist, QString> importIptvPlaylistFile(const ServerConfig& server, std::vector<IptvChannel>& channels) const;
    void loadWebDavService(const ServiceCard& card, const QString& password);
    void clearWebDavState();
    void loadWebDavDirectory(const QUrl& url);
    void saveWebDavCredentials(const ServerConfig& server, const QString& password);
    std::optional<QString> loadWebDavPassword(const ServerConfig& server);
    QUrl childWebDavUrl(const QString& name, bool directory) const;
    QString uniqueLocalPath(const QString& directory, const QString& name) const;
    void enqueueWebDavUploadFile(const QString& localPath, const QUrl& remoteUrl);
    void wireWebDavCertificatePrompt();
    void wireUsageSignals();
    bool accumulateUsage(const ServerConfig& server,
                         bool privacyMode,
                         qint64 watchSeconds,
                         qint64 bytesReceived,
                         qint64 bytesSent,
                         NetworkTrafficCategory trafficCategory = NetworkTrafficCategory::Normal);
    void flushPendingUsageStats(bool refreshAfterFlush);
    void refreshUsageStats();
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
    void beginHomeLoading();
    void endHomeLoading();
    void setLibraryItemsLoading(bool value);
    void setError(QString message);
    void setSession(UserSession session);
    void saveSession();
    void wireCertificatePrompt(MediaServiceClient& client);
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
    QString m_webDavPassword;
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
    int m_homeLoadingRequests { 0 };
    bool m_libraryItemsLoading { false };
    QString m_serverSearchText;
    QString m_activeServerSearchTerm;
    bool m_serverSearchLoading { false };
    int m_serverSearchNextStartIndex { 0 };
    int m_serverSearchPageSize { 60 };
    bool m_serverSearchHasMore { false };
    int m_serverSearchRequestGeneration { 0 };
    bool m_detailsReturnToSearch { false };
    QString m_errorMessage;
    std::optional<UserSession> m_session;
    std::optional<ServiceCard> m_pendingServiceCard;
    std::optional<ServiceCard> m_currentIptvCard;
    std::optional<IptvPlaylist> m_currentIptvPlaylist;
    QString m_currentIptvChannelId;
    std::optional<ServiceCard> m_currentWebDavCard;
    QUrl m_webDavCurrentUrl;
    std::vector<QUrl> m_webDavHistory;
    std::optional<MediaLibrary> m_currentLibrary;
    QString m_currentMediaParentId;
    QString m_currentMediaParentName;
    std::vector<std::pair<QString, QString>> m_mediaParentHistory;
    std::optional<MediaItem> m_selectedItem;
    std::optional<MediaItem> m_selectedSeason;
    QUrl m_currentPlaybackUrl;
    QString m_currentMediaSourceId;
    QString m_currentPlaySessionId;
    QString m_playbackHttpUsername;
    QString m_playbackHttpPassword;
    bool m_playbackAllowInsecureTls { false };
    QString m_webDavPlaybackStreamId;
    double m_currentPlaybackStartSeconds { 0.0 };
    double m_lastPlaybackReportSeconds { -1.0 };
    bool m_playbackStartedReported { false };
    bool m_playbackUsageActive { false };
    bool m_playbackUsagePaused { false };
    std::optional<ServerConfig> m_playbackUsageServer;
    QDateTime m_playbackUsageLastWallClock;
    QString m_currentView { QStringLiteral("services") };
    int m_nextItemStartIndex { 0 };
    int m_itemPageSize { 80 };
    bool m_hasMoreMediaItems { true };
    int m_seriesRequestGeneration { 0 };
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
    WebDavDownloadPlanner m_webDavDownloadPlanner;
    WebDavPlaybackProxy m_webDavPlaybackProxy;
    TransferManager m_transferManager;
    SessionRepository m_repository;
    ScheduledPlaybackManager m_scheduledPlaybackManager;
    ServiceCardListModel m_services;
    ServiceCardListModel m_privacyCards;
    ServiceCardListModel m_scheduledEmbySources;
    ScheduledPlaybackTaskListModel m_scheduledPlaybackTasks;
    MediaLibraryListModel m_libraries;
    MediaItemListModel m_continueItems;
    MediaItemListModel m_items;
    MediaItemListModel m_serverSearchResults;
    MediaItemListModel m_seriesSeasons;
    MediaItemListModel m_seriesEpisodes;
    IptvChannelListModel m_iptvChannels;
    WebDavItemListModel m_webDavItems;
    DailyUsageStatsListModel m_usageStats;
    qint64 m_historyTotalWatchSeconds { 0 };
    qint64 m_historyTotalNetworkBytes { 0 };
    qint64 m_historyTotalNetworkBytesIn { 0 };
    qint64 m_historyTotalNetworkBytesOut { 0 };
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
    std::function<void(bool)> m_pendingCertificateReply;
    std::function<void(bool)> m_pendingDownloadWarningReply;
    std::function<void()> m_pendingFolderDownload;
};
