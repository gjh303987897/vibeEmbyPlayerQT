#include "database/SessionRepository.h"
#include "services/iptv/IptvParser.h"
#include "services/iptv/IptvProbe.h"
#include "services/iptv/IptvStreamSelector.h"
#include "services/iptv/IptvStreamWorkflow.h"

#include <QFile>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QProcess>
#include <QStandardPaths>
#include <QtTest>

class ProbeEndpoint final : public QTcpServer {
public:
    QHash<QByteArray, QByteArray> bodies;
    QList<QByteArray> requests;
    ProbeEndpoint() {
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (auto* socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    auto request = socket->property("request").toByteArray() + socket->readAll();
                    socket->setProperty("request", request);
                    if (!request.contains("\r\n\r\n") || socket->property("handled").toBool()) return;
                    socket->setProperty("handled", true);
                    const auto path = request.split(' ').value(1);
                    requests.push_back(path);
                    if (path == "/hang") return;
                    if (path == "/redirect") {
                        socket->write("HTTP/1.1 302 Found\r\nLocation: /live/media.m3u8\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    } else if (path.startsWith("/reject-") && request.contains("Range:")) {
                        socket->write("HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    } else {
                        auto body = bodies.value(path);
                        if (path == "/ranges") {
                            const auto match = QRegularExpression("Range: bytes=(\\d+)-(\\d+)").match(QString::fromLatin1(request));
                            const auto offset = match.captured(1).toLongLong();
                            const auto end = std::min(match.captured(2).toLongLong(), body.size() - 1);
                            socket->write("HTTP/1.1 206 Partial Content\r\nContent-Range: bytes " + QByteArray::number(offset) + "-" +
                                QByteArray::number(end) + "/" + QByteArray::number(body.size()) + "\r\n");
                            body = body.mid(offset, end - offset + 1);
                        } else
                        socket->write(bodies.contains(path) ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n");
                        socket->write("Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                    }
                    socket->disconnectFromHost();
                });
            }
        });
    }
    QUrl url(const QString& path) const { return QUrl(QString("http://127.0.0.1:%1%2").arg(serverPort()).arg(path)); }
};

class IptvMultiSourceTest final : public QObject {
    Q_OBJECT
private slots:
    void sharedWorkflowPreservesPlaybackAndVariantFailure();
    void mergesIdentityAndPreservesRoutes();
    void choosesQualityAndActualBitrate();
    void expiresAndDeprioritizesFailedRoutes();
    void resolvesHlsVariantsAndKeepsExternalTracks();
    void persistsPerRouteInformation();
    void readsLegacySchemaWithoutMigration();
    void boundsNetworkReadAndCancelsReplacement();
    void failedHlsVariantFallsBackAndExpires();
    void samplesHlsAndDirectStreams();
    void boundsHlsTraversalAndReportsFailures();
};

void IptvMultiSourceTest::sharedWorkflowPreservesPlaybackAndVariantFailure()
{
    IptvChannel channel {.id = "channel",.streamUrl = "https://example.test/master.m3u8"};
    channel.streamUrls = {channel.streamUrl};
    IptvProbeResult probe {.available = true,.width = 1920,.height = 1080,.codec = "h264",.state = "available"};
    probe.variants = {{"https://example.test/hd.m3u8",1920,1080,5000000,"h264"},
                      {"https://example.test/sd.m3u8",640,360,500000,"h264"}};
    IptvStreamWorkflow::applyProbe(channel,channel.streamUrl,probe);
    const auto selected = IptvStreamSelector::select(channel,"quality");
    IptvStreamWorkflow::markFailed(channel,selected);
    QVERIFY(IptvStreamSelector::select(channel,"quality").playbackUrl != selected.playbackUrl);
    QVERIFY(IptvStreamWorkflow::applyPlayback(channel,selected.sourceUrl,selected.playbackUrl,"1280x720","hevc","2 Mbps"));
    QCOMPARE(channel.width,1280); QCOMPARE(channel.bitrate,2000000);
    IptvStreamWorkflow::applyProbe(channel,channel.streamUrl,probe);
    QCOMPARE(channel.streamInfo.front().width,1280);
    QVERIFY(!channel.streamInfo.front().variants.front().failedAt.isValid());
    QVERIFY(!IptvStreamWorkflow::applyPlayback(channel,selected.sourceUrl,selected.playbackUrl,"","",""));
    const auto rows = IptvStreamWorkflow::rows(channel,"quality");
    QVERIFY(!rows.isEmpty());
    QVERIFY(!rows.at(0).toObject().contains("url"));
    const IptvStreamSelector::Selection master {.sourceUrl = channel.streamUrl,.playbackUrl = channel.streamUrl};
    const auto actual = IptvStreamWorkflow::rows(channel,"quality",IptvStreamSelector::selectionId(master),&master);
    QCOMPARE(actual.size(),rows.size() + 1);
    QCOMPARE(std::ranges::count_if(actual,[](const auto& value) { return value.toObject()["current"].toBool(); }),1);
}

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
    const auto externalResult = IptvProbe::parse(external, 200, "quality", base);
    QVERIFY(externalResult.variants.empty());
    QCOMPARE(externalResult.width, 1920);
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
        .checkedAt = QDateTime::currentDateTimeUtc(), .variants = {{"https://test/hd", 1920, 1080, 5'000'000, "h264", {}, "measured"}},
        .qualityCheckedAt = QDateTime::currentDateTimeUtc()}};
    QVERIFY(repository.updateIptvChannelProbe("playlist", "channel", 1920, 1080, 5'000'000, "h264", "playback", infos).has_value());
    SessionRepository reader("iptv-read", path, SessionRepository::OpenMode::ReadOnly);
    const auto loaded = reader.loadIptvChannels("service");
    QVERIFY(loaded.has_value());
    QCOMPARE(loaded->size(), size_t(1));
    QCOMPARE(loaded->front().streamUrls, channel.streamUrls);
    QCOMPARE(loaded->front().streamInfo[0].variants[0].url, "https://test/hd");
    QVERIFY(IptvStreamSelector::fresh(loaded->front().streamInfo[0]));
    QCOMPARE(loaded->front().streamInfo[0].variants[0].probeState, QString("measured"));
    QCOMPARE(loaded->front().streamInfo[0].qualityCheckedAt, infos[0].qualityCheckedAt);
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
            socket->write(QByteArray(1024 * 1024, 'x'));
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
    probe.probe("deep", QUrl(base + "/large"), "quality", true);
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 3, 3500); // Shorter than the 4-second request timeout.
    QVERIFY(qvariant_cast<IptvProbeResult>(completed[2][2]).detailed);
}

void IptvMultiSourceTest::samplesHlsAndDirectStreams()
{
    const auto ffmpeg = QStandardPaths::findExecutable("ffmpeg");
    if (ffmpeg.isEmpty() || QStandardPaths::findExecutable("ffprobe").isEmpty()) QSKIP("Requires FFmpeg and FFprobe");
    QTemporaryDir temporary;
    QProcess generator;
    const auto generate = [&](const QString& file, const QStringList& format) {
        QStringList arguments {"-v", "error", "-f", "lavfi", "-i", "testsrc2=size=320x180:rate=10",
            "-t", "1", "-c:v", "libx264", "-preset", "ultrafast", "-threads", "1"};
        arguments.append(format);
        arguments << "-y" << temporary.filePath(file);
        generator.start(ffmpeg, arguments);
        if (!generator.waitForFinished(10000) || generator.exitCode() != 0) return QByteArray {};
        QFile output(temporary.filePath(file));
        if (!output.open(QIODevice::ReadOnly)) return QByteArray {};
        return output.readAll();
    };
    const auto ts = generate("sample.ts", {"-f", "mpegts"});
    const auto mp4 = generate("sample.mp4", {"-movflags", "frag_keyframe+empty_moov+default_base_moof"});
    QVERIFY(!ts.isEmpty());
    const auto boundary = mp4.indexOf("moof") - 4;
    QVERIFY(boundary > 0);
    ProbeEndpoint server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.bodies = {{"/live/media.m3u8", "#EXTM3U\n#EXT-X-TARGETDURATION:1\n#EXTINF:1,\nsample.ts\n"},
        {"/live/sample.ts", ts}, {"/direct", ts}, {"/reject-range", ts},
        {"/fmp4.m3u8", "#EXTM3U\n#EXT-X-MAP:URI=\"init.mp4\"\n#EXTINF:1,\nfragment.m4s\n"},
        {"/init.mp4", mp4.first(boundary)}, {"/fragment.m4s", mp4.mid(boundary)}, {"/ranges", mp4},
        {"/byterange.m3u8", QString("#EXTM3U\n#EXT-X-MAP:URI=\"ranges\",BYTERANGE=\"%1@0\"\n#EXTINF:1,\n#EXT-X-BYTERANGE:%2@%1\nranges\n")
            .arg(boundary).arg(mp4.size() - boundary).toUtf8()},
        {"/master", "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=900000\nlive/media.m3u8\n#EXT-X-STREAM-INF:BANDWIDTH=1000000\nfmp4.m3u8\n"}};
    server.bodies.insert("/reject-master", server.bodies.value("/master"));
    IptvProbe probe;
    QSignalSpy finished(&probe, &IptvProbe::finished);
    int heartbeats = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, [&] { ++heartbeats; });
    heartbeat.start(5);
    for (const auto& path : {"/redirect", "/direct", "/reject-range", "/fmp4.m3u8", "/byterange.m3u8", "/master", "/reject-master"}) {
        finished.clear();
        probe.probe("channel", server.url(path), "quality", true);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 10000);
        const auto result = qvariant_cast<IptvProbeResult>(finished[0][2]);
        QCOMPARE(result.width, 320);
        QCOMPARE(result.height, 180);
        QCOMPARE(result.codec, QString("h264"));
        QVERIFY(result.detailed);
        if (QString(path).endsWith("master")) {
            QCOMPARE(result.variants.size(), size_t(2));
            QCOMPARE(result.variants[1].height, 180);
        }
    }
    QVERIFY(heartbeats > 10);
    QCOMPARE(server.requests.count("/reject-range"), 2);
}

void IptvMultiSourceTest::boundsHlsTraversalAndReportsFailures()
{
    ProbeEndpoint server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    server.bodies = {{"/encrypted", "#EXTM3U\n#EXT-X-KEY:METHOD=AES-128,URI=\"key\"\n#EXTINF:1,\nsecret.ts\n"},
        {"/loop", "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=10\nloop\n"},
        {"/slow", "#EXTM3U\n#EXTINF:1,\nhang\n"},
        {"/good", "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=2000000,RESOLUTION=1280x720\nvideo\n"}};
    IptvProbe probe;
    QSignalSpy finished(&probe, &IptvProbe::finished);
    for (const auto& path : {"/encrypted", "/missing", "/loop"}) {
        finished.clear();
        probe.probe("channel", server.url(path), "quality", true);
        QTRY_COMPARE(finished.size(), 1);
        const auto result = qvariant_cast<IptvProbeResult>(finished[0][2]);
        if (QString(path) == "/encrypted") QCOMPARE(result.state, QString("encrypted"));
        if (QString(path) == "/missing") QCOMPARE(result.state, QString("unavailable"));
        if (QString(path) == "/loop") QCOMPARE(result.variants.front().probeState, QString("probe-limit"));
    }
    QVERIFY(!server.requests.contains("/key"));
    QVERIFY(!server.requests.contains("/secret.ts"));
    QVERIFY(server.requests.count("/loop") <= 5);
    finished.clear();
    probe.probe("canceled", server.url("/slow"), "quality", true);
    QTRY_VERIFY(server.requests.contains("/hang"));
    probe.cancel();
    probe.probe("replacement", server.url("/good"), "quality", true);
    QTRY_COMPARE(finished.size(), 1);
    QCOMPARE(finished[0][0].toString(), QString("replacement"));
    QCOMPARE(qvariant_cast<IptvProbeResult>(finished[0][2]).height, 720);
}

QTEST_GUILESS_MAIN(IptvMultiSourceTest)
#include "IptvMultiSourceTest.moc"
