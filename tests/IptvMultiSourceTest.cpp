#include "database/SessionRepository.h"
#include "services/iptv/IptvParser.h"
#include "services/iptv/IptvProbe.h"
#include "services/iptv/IptvStreamSelector.h"

#include <QFile>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

class IptvMultiSourceTest final : public QObject {
    Q_OBJECT
private slots:
    void mergesIdentityAndPreservesRoutes();
    void choosesQualityAndActualBitrate();
    void expiresAndDeprioritizesFailedRoutes();
    void resolvesHlsVariantsAndKeepsExternalTracks();
    void persistsPerRouteInformation();
    void readsLegacySchemaWithoutMigration();
    void boundsNetworkReadAndCancelsReplacement();
    void failedHlsVariantFallsBackAndExpires();
};

void IptvMultiSourceTest::mergesIdentityAndPreservesRoutes()
{
    QTemporaryDir dir;
    QFile file(dir.filePath("channels.m3u"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("#EXTM3U\n#EXTINF:-1 tvg-id=\"one\" group-title=\"News\",Channel\nhttps://test/a\n"
               "#EXTINF:-1 tvg-id=\"one\" group-title=\"Other\",Alias\nhttps://test/b\n"
               "#EXTINF:-1 tvg-id=\"two\" group-title=\"News\",Channel\nhttps://test/c\n"
               "#EXTINF:-1 group-title=\"Music\",Radio\nhttps://test/d\n"
               "#EXTINF:-1 group-title=\"music\",  radio  \nhttps://test/e\n");
    file.close();
    auto parsed = IptvParser::parseFile(file.fileName());
    QVERIFY(parsed.has_value());
    const auto originalId = parsed->front().id;
    parsed->front().id = "legacy-id";
    auto channels = IptvParser::mergeChannels(*parsed);
    QCOMPARE(channels.size(), size_t(3));
    QCOMPARE(channels[0].id, "legacy-id");
    QCOMPARE(channels[0].streamUrls, (QStringList {"https://test/a", "https://test/b"}));
    QCOMPARE(channels[2].streamUrls.size(), 2);
    channels.push_back(channels[0]);
    QCOMPARE(IptvParser::mergeChannels(channels)[0].streamUrls.size(), 2);
    QVERIFY(!originalId.isEmpty());
}

void IptvMultiSourceTest::choosesQualityAndActualBitrate()
{
    const auto now = QDateTime::currentDateTimeUtc();
    IptvChannel channel {.streamUrl = "unknown", .streamUrls = {"unknown", "hd", "sd"}};
    channel.streamInfo = {
        {.url = "hd", .available = true, .width = 1920, .height = 1080,
         .bitrate = 1'000'000, .checkedAt = now},
        {.url = "sd", .available = true, .width = 1280, .height = 720,
         .bitrate = 2'000'000, .checkedAt = now}};
    QCOMPARE(IptvStreamSelector::select(channel, "quality", now).playbackUrl, "hd");
    // A better codec can make HD use fewer bytes than SD.
    QCOMPARE(IptvStreamSelector::select(channel, "dataSaver", now).playbackUrl, "hd");
    channel.streamInfo[0].bitrate = 5'000'000;
    QCOMPARE(IptvStreamSelector::select(channel, "dataSaver", now).playbackUrl, "sd");
}

void IptvMultiSourceTest::expiresAndDeprioritizesFailedRoutes()
{
    const auto now = QDateTime::currentDateTimeUtc();
    IptvChannel channel {.streamUrls = {"failed", "unknown", "good"}};
    channel.streamInfo = {{.url = "failed", .checkedAt = now},
        {.url = "good", .available = true, .checkedAt = now}};
    QCOMPARE(IptvStreamSelector::select(channel, "quality", now).sourceUrl, "good");
    channel.streamInfo[1].checkedAt = now.addSecs(-6 * 3600);
    QCOMPARE(IptvStreamSelector::select(channel, "quality", now).sourceUrl, "unknown");
    channel.streamInfo[0].checkedAt = now.addSecs(-60);
    QCOMPARE(IptvStreamSelector::select(channel, "quality", now).sourceUrl, "failed");
}

void IptvMultiSourceTest::failedHlsVariantFallsBackAndExpires()
{
    const auto now = QDateTime::currentDateTimeUtc();
    IptvChannel channel {.streamUrls = {"master", "unknown", "backup"}};
    channel.streamInfo = {{.url = "master", .available = true, .checkedAt = now,
        .variants = {{"hd", 1920, 1080, 5'000'000, "h264", now}, {"sd", 1280, 720, 1'000'000, "h264"}}},
        {.url = "backup", .available = true, .width = 640, .height = 360, .checkedAt = now}};
    const auto ranked = IptvStreamSelector::candidates(channel, "quality", now);
    QCOMPARE(ranked.size(), size_t(4));
    QCOMPARE(ranked.front().playbackUrl, "sd");
    QCOMPARE(ranked.back().playbackUrl, "hd");
    channel.streamInfo[0].variants[1].failedAt = now;
    QCOMPARE(IptvStreamSelector::select(channel, "quality", now).playbackUrl, "backup");
    QCOMPARE(IptvStreamSelector::select(channel, "quality", now.addSecs(61)).playbackUrl, "hd");
    const auto restored = IptvStreamSelector::deserialize(IptvStreamSelector::serialize(channel.streamInfo));
    QCOMPARE(restored[0].variants[0].failedAt.toMSecsSinceEpoch(), now.toMSecsSinceEpoch());
    const auto later = IptvStreamSelector::candidates(channel, "quality", now.addSecs(61));
    const auto sd = std::ranges::find(later, QString("sd"), &IptvStreamSelector::Selection::playbackUrl);
    QVERIFY(sd != later.end());
    QCOMPARE(IptvStreamSelector::selectionId(ranked.front()), IptvStreamSelector::selectionId(*sd));
    QVERIFY(IptvStreamSelector::selectionId(later.front()) != IptvStreamSelector::selectionId(*sd));
}

void IptvMultiSourceTest::resolvesHlsVariantsAndKeepsExternalTracks()
{
    const QByteArray manifest = "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080,CODECS=\"avc1\"\nhd/index.m3u8\n"
        "#EXT-X-STREAM-INF:BANDWIDTH=1000000,RESOLUTION=640x360\n../sd.m3u8\n";
    const QUrl base("https://test/live/master.m3u8");
    const auto result = IptvProbe::parse(manifest, 200, "quality", base);
    QVERIFY(result.available);
    QCOMPARE(result.width, 1920);
    QCOMPARE(result.height, 1080);
    QCOMPARE(result.variants.size(), size_t(2));
    QCOMPARE(result.variants[0].url, "https://test/live/hd/index.m3u8");
    IptvChannel channel {.streamUrl = base.toString(), .streamUrls = {base.toString()}};
    channel.streamInfo = {{.url = base.toString(), .available = true,
        .checkedAt = QDateTime::currentDateTimeUtc(), .variants = result.variants}};
    QCOMPARE(IptvStreamSelector::select(channel, "dataSaver").playbackUrl, "https://test/sd.m3u8");
    auto external = manifest;
    external.replace("CODECS=\"avc1\"", "AUDIO=\"audio\",CODECS=\"avc1\"");
    QVERIFY(IptvProbe::parse(external, 200, "quality", base).variants.empty());
    QVERIFY(!IptvProbe::parse(manifest, 404, "quality", base).available);
}

void IptvMultiSourceTest::persistsPerRouteInformation()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("session.sqlite");
    SessionRepository repository("iptv-roundtrip", path);
    QVERIFY(repository.initialize().has_value());
    ServerConfig server {.id = "service", .name = "IPTV", .baseUrl = "file:///test.m3u",
        .username = "", .serviceType = ServiceType::IPTV};
    IptvPlaylist playlist {"playlist", "service", "IPTV", "local", "test.m3u", "test.m3u", "2026-10-12"};
    IptvChannel channel {.id = "channel", .name = "Channel", .groupName = "News", .streamUrl = "https://test/master",
        .streamUrls = {"https://test/master", "https://test/backup"}};
    QVERIFY(repository.saveIptvPlaylist(server, playlist, {channel}).has_value());
    const std::vector<IptvStreamInfo> infos {{.url = channel.streamUrl, .available = true,
        .width = 1920, .height = 1080, .bitrate = 5'000'000, .codec = "h264", .state = "playback",
        .checkedAt = QDateTime::currentDateTimeUtc(), .variants = {{"https://test/hd", 1920, 1080, 5'000'000, "h264"}}}};
    QVERIFY(repository.updateIptvChannelProbe("playlist", "channel", 1920, 1080, 5'000'000, "h264", "playback", infos).has_value());
    SessionRepository reader("iptv-read", path, SessionRepository::OpenMode::ReadOnly);
    const auto loaded = reader.loadIptvChannels("service");
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->size(), size_t(1));
    QCOMPARE(loaded->front().streamUrls, channel.streamUrls);
    QCOMPARE(loaded->front().streamInfo[0].variants[0].url, "https://test/hd");
    QVERIFY(IptvStreamSelector::fresh(loaded->front().streamInfo[0]));
}

void IptvMultiSourceTest::readsLegacySchemaWithoutMigration()
{
    QTemporaryDir dir;
    const auto path = dir.filePath("legacy.sqlite");
    {
        auto database = QSqlDatabase::addDatabase("QSQLITE", "legacy-fixture");
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec("CREATE TABLE iptv_playlists(id TEXT, service_id TEXT, name TEXT, source_type TEXT, source_path TEXT, imported_path TEXT, imported_at TEXT)"));
        QVERIFY(query.exec("INSERT INTO iptv_playlists VALUES('p','s','TV','local','a','a','2026-10-12')"));
        QVERIFY(query.exec("CREATE TABLE iptv_channels(id TEXT, playlist_id TEXT, name TEXT, group_name TEXT, logo_url TEXT, stream_url TEXT, sort_order INTEGER)"));
        QVERIFY(query.exec("INSERT INTO iptv_channels VALUES('c','p','TV','News','','https://test/a',0)"));
    }
    QSqlDatabase::removeDatabase("legacy-fixture");
    SessionRepository reader("iptv-legacy-read", path, SessionRepository::OpenMode::ReadOnly);
    const auto loaded = reader.loadIptvChannels("s");
    QVERIFY2(loaded.has_value(), loaded ? "" : qPrintable(loaded.error()));
    QCOMPARE(loaded->front().streamUrls, (QStringList {"https://test/a"}));
    const auto database = QSqlDatabase::database("iptv-legacy-read");
    QVERIFY(!database.record("iptv_channels").contains("stream_info"));
}

void IptvMultiSourceTest::boundsNetworkReadAndCancelsReplacement()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&] {
        auto* socket = server.nextPendingConnection();
        connect(socket, &QTcpSocket::readyRead, socket, [socket] {
            const auto request = socket->readAll();
            if (!request.contains("\r\n\r\n")) return;
            if (request.startsWith("GET /hang")) return;
            // Advertise a never-ending response and leave the socket open.
            // Completion before the six-second timeout requires the sample cap.
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/octet-stream\r\nContent-Length: 1073741824\r\n\r\n");
            socket->write(QByteArray(256 * 1024, 'x'));
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    });
    IptvProbe probe;
    QSignalSpy completed(&probe, &IptvProbe::finished);
    const auto base = QString("http://127.0.0.1:%1").arg(server.serverPort());
    probe.probe("old", QUrl(base + "/hang"), "quality");
    QTest::qWait(30);
    probe.probe("new", QUrl(base + "/large"), "quality");
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 5000);
    QCOMPARE(completed[0][0].toString(), "new");
    probe.probe("unsupported-old", QUrl("file:///same"), "quality");
    probe.probe("unsupported-new", QUrl("file:///same"), "quality");
    QTRY_COMPARE(completed.count(), 2);
    QCOMPARE(completed[1][0].toString(), "unsupported-new");
}

QTEST_GUILESS_MAIN(IptvMultiSourceTest)
#include "IptvMultiSourceTest.moc"
