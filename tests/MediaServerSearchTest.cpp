#include "network/NetworkClient.h"
#include "services/emby/EmbyClient.h"
#include "services/jellyfin/JellyfinClient.h"

#include <QHash>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUrlQuery>

#include <expected>
#include <optional>
#include <utility>

namespace {
class LocalMediaServer final : public QObject {
    Q_OBJECT

public:
    explicit LocalMediaServer(QByteArray responseBody = {}, QObject* parent = nullptr)
        : QObject(parent)
        , m_responseBody(responseBody.isEmpty()
              ? QByteArrayLiteral(R"({"Items":[{"Id":"movie-1","Name":"Alien","Type":"Movie","ProductionYear":1979,"ImageTags":{"Primary":"poster-tag"},"UserData":{"PlayedPercentage":25.0}}],"TotalRecordCount":1})")
              : std::move(responseBody))
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (auto* socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket]() {
                    auto& buffer = m_buffers[socket];
                    buffer += socket->readAll();
                    const auto headerEnd = buffer.indexOf(QByteArrayLiteral("\r\n\r\n"));
                    if (headerEnd < 0) {
                        return;
                    }

                    const auto headerLines = buffer.first(headerEnd).split('\n');
                    const auto requestParts = headerLines.value(0).trimmed().split(' ');
                    m_requestTarget = requestParts.value(1);
                    ++m_requestCount;
                    m_headers.clear();
                    for (qsizetype index = 1; index < headerLines.size(); ++index) {
                        const auto line = headerLines.at(index).trimmed();
                        const auto separator = line.indexOf(':');
                        if (separator <= 0) {
                            continue;
                        }
                        m_headers.insert(line.first(separator).toLower(), line.mid(separator + 1).trimmed());
                    }
                    m_buffers.remove(socket);

                    const auto response = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ")
                        + QByteArray::number(m_responseBody.size()) + QByteArrayLiteral("\r\n\r\n") + m_responseBody;
                    socket->write(response);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, [this, socket]() {
                    m_buffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }

    bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    QString baseUrl() const
    {
        return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort());
    }

    QByteArray requestTarget() const
    {
        return m_requestTarget;
    }

    QByteArray header(const QByteArray& name) const
    {
        return m_headers.value(name.toLower());
    }

    int requestCount() const
    {
        return m_requestCount;
    }

private:
    QTcpServer m_server;
    QHash<QTcpSocket*, QByteArray> m_buffers;
    QHash<QByteArray, QByteArray> m_headers;
    QByteArray m_responseBody;
    QByteArray m_requestTarget;
    int m_requestCount { 0 };
};

UserSession sessionFor(const LocalMediaServer& server, ServiceType serviceType)
{
    return UserSession {
        .server = ServerConfig {
            .id = serviceType == ServiceType::Emby ? QStringLiteral("emby-server") : QStringLiteral("jellyfin-server"),
            .name = serviceType == ServiceType::Emby ? QStringLiteral("Emby Test") : QStringLiteral("Jellyfin Test"),
            .baseUrl = server.baseUrl(),
            .username = QStringLiteral("tester"),
            .serviceType = serviceType,
        },
        .userId = QStringLiteral("user-id"),
        .username = QStringLiteral("tester"),
        .accessToken = QStringLiteral("secret-token"),
    };
}

void verifyParsedResult(const std::optional<ItemResult>& result)
{
    QVERIFY(result.has_value());
    QVERIFY(result->has_value());
    QCOMPARE(result->value().size(), size_t { 1 });
    QCOMPARE(result->value().front().id, QStringLiteral("movie-1"));
    QCOMPARE(result->value().front().name, QStringLiteral("Alien"));
    QCOMPARE(result->value().front().itemType, QStringLiteral("Movie"));
    QCOMPARE(result->value().front().playedPercentage, 25.0);
    QVERIFY(result->value().front().imageUrl.contains(QStringLiteral("/Items/movie-1/Images/Primary")));
}
}

class MediaServerSearchTest final : public QObject {
    Q_OBJECT

private slots:
    void embySearchesCurrentUserRootRecursively();
    void jellyfinSearchesCurrentUserRootRecursively();
    void embyRequestsSuggestedSeries();
    void jellyfinRequestsSuggestedSeries();
    void embyDetailsPreferItemLogoArtwork();
    void embyEpisodeDetailsUseInheritedSeriesLogoArtwork();
    void rejectsBlankSearchTermsWithoutRequests();
    void embyPlaybackPrefersFullDefaultSubtitleOverForcedSelection();
    void jellyfinPlaybackKeepsNonForcedServerSubtitleSelection();
    void playbackWithoutValidSubtitleUsesPlayerAutomaticSelection();
};

void MediaServerSearchTest::embySearchesCurrentUserRootRecursively()
{
    LocalMediaServer server;
    QVERIFY(server.listen());
    NetworkClient networkClient;
    EmbyClient client(networkClient);
    std::optional<ItemResult> result;

    client.searchVideoItems(sessionFor(server, ServiceType::Emby),
                            QStringLiteral("  Alien  "),
                            -3,
                            0,
                            [&result](ItemResult value) {
        result = std::move(value);
    });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
    verifyParsedResult(result);

    const QUrl requestUrl(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(server.requestTarget()));
    const QUrlQuery query(requestUrl);
    QCOMPARE(requestUrl.path(), QStringLiteral("/Users/user-id/Items"));
    QCOMPARE(query.queryItemValue(QStringLiteral("SearchTerm")), QStringLiteral("Alien"));
    QCOMPARE(query.queryItemValue(QStringLiteral("Recursive")), QStringLiteral("true"));
    QCOMPARE(query.queryItemValue(QStringLiteral("IncludeItemTypes")), QStringLiteral("Movie,Series,Episode,Video"));
    QCOMPARE(query.queryItemValue(QStringLiteral("StartIndex")), QStringLiteral("0"));
    QCOMPARE(query.queryItemValue(QStringLiteral("Limit")), QStringLiteral("1"));
    QVERIFY(!query.hasQueryItem(QStringLiteral("ParentId")));
    QVERIFY(!query.hasQueryItem(QStringLiteral("MediaTypes")));
    QVERIFY(server.header(QByteArrayLiteral("authorization")).startsWith(QByteArrayLiteral("Emby ")));
    QVERIFY(server.header(QByteArrayLiteral("authorization")).contains(QByteArrayLiteral("Token=\"secret-token\"")));
    QCOMPARE(server.header(QByteArrayLiteral("x-emby-token")), QByteArrayLiteral("secret-token"));
}

void MediaServerSearchTest::jellyfinSearchesCurrentUserRootRecursively()
{
    LocalMediaServer server;
    QVERIFY(server.listen());
    NetworkClient networkClient;
    JellyfinClient client(networkClient);
    std::optional<ItemResult> result;

    client.searchVideoItems(sessionFor(server, ServiceType::Jellyfin),
                            QStringLiteral("  Alien  "),
                            -3,
                            0,
                            [&result](ItemResult value) {
        result = std::move(value);
    });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
    verifyParsedResult(result);

    const QUrl requestUrl(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(server.requestTarget()));
    const QUrlQuery query(requestUrl);
    QCOMPARE(requestUrl.path(), QStringLiteral("/Items"));
    QCOMPARE(query.queryItemValue(QStringLiteral("userId")), QStringLiteral("user-id"));
    QCOMPARE(query.queryItemValue(QStringLiteral("searchTerm")), QStringLiteral("Alien"));
    QCOMPARE(query.queryItemValue(QStringLiteral("recursive")), QStringLiteral("true"));
    QCOMPARE(query.queryItemValue(QStringLiteral("includeItemTypes")), QStringLiteral("Movie,Series,Episode,Video"));
    QCOMPARE(query.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("0"));
    QCOMPARE(query.queryItemValue(QStringLiteral("limit")), QStringLiteral("1"));
    QVERIFY(!query.hasQueryItem(QStringLiteral("parentId")));
    QVERIFY(!query.hasQueryItem(QStringLiteral("mediaTypes")));
    QVERIFY(server.header(QByteArrayLiteral("authorization")).startsWith(QByteArrayLiteral("MediaBrowser ")));
    QVERIFY(server.header(QByteArrayLiteral("authorization")).contains(QByteArrayLiteral("Token=\"secret-token\"")));
    QCOMPARE(server.header(QByteArrayLiteral("x-emby-token")), QByteArrayLiteral("secret-token"));
}

void MediaServerSearchTest::embyRequestsSuggestedSeries()
{
    LocalMediaServer server(QByteArrayLiteral(
        R"({"Items":[{"Id":"series-1","Name":"Example Show","Type":"Series","ImageTags":{"Primary":"poster-tag"},"BackdropImageTags":["backdrop-tag"]}],"TotalRecordCount":1})"));
    QVERIFY(server.listen());
    NetworkClient networkClient;
    EmbyClient client(networkClient);
    std::optional<ItemResult> result;

    client.fetchSuggestedSeries(sessionFor(server, ServiceType::Emby),
                                8,
                                [&result](ItemResult value) {
        result = std::move(value);
    });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
    QVERIFY(result->has_value());
    QCOMPARE(result->value().size(), size_t { 1 });
    QCOMPARE(result->value().front().id, QStringLiteral("series-1"));
    QCOMPARE(result->value().front().itemType, QStringLiteral("Series"));
    QVERIFY(!result->value().front().backdropImageUrl.isEmpty());

    const QUrl requestUrl(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(server.requestTarget()));
    const QUrlQuery query(requestUrl);
    QCOMPARE(requestUrl.path(), QStringLiteral("/Users/user-id/Suggestions"));
    QCOMPARE(query.queryItemValue(QStringLiteral("Recursive")), QStringLiteral("true"));
    QCOMPARE(query.queryItemValue(QStringLiteral("IncludeItemTypes")), QStringLiteral("Series"));
    QCOMPARE(query.queryItemValue(QStringLiteral("Limit")), QStringLiteral("8"));
    QCOMPARE(query.queryItemValue(QStringLiteral("ImageTypeLimit")), QStringLiteral("2"));
    QCOMPARE(query.queryItemValue(QStringLiteral("EnableImageTypes")), QStringLiteral("Primary,Backdrop"));
    QCOMPARE(query.queryItemValue(QStringLiteral("EnableUserData")), QStringLiteral("true"));
    QVERIFY(server.header(QByteArrayLiteral("authorization")).startsWith(QByteArrayLiteral("Emby ")));
    QCOMPARE(server.header(QByteArrayLiteral("x-emby-token")), QByteArrayLiteral("secret-token"));
}

void MediaServerSearchTest::jellyfinRequestsSuggestedSeries()
{
    LocalMediaServer server(QByteArrayLiteral(
        R"({"Items":[{"Id":"series-1","Name":"Example Show","Type":"Series","ImageTags":{"Primary":"poster-tag"},"BackdropImageTags":["backdrop-tag"]}],"TotalRecordCount":1})"));
    QVERIFY(server.listen());
    NetworkClient networkClient;
    JellyfinClient client(networkClient);
    std::optional<ItemResult> result;

    client.fetchSuggestedSeries(sessionFor(server, ServiceType::Jellyfin),
                                8,
                                [&result](ItemResult value) {
        result = std::move(value);
    });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
    QVERIFY(result->has_value());
    QCOMPARE(result->value().size(), size_t { 1 });
    QCOMPARE(result->value().front().id, QStringLiteral("series-1"));
    QCOMPARE(result->value().front().itemType, QStringLiteral("Series"));
    QVERIFY(!result->value().front().backdropImageUrl.isEmpty());

    const QUrl requestUrl(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(server.requestTarget()));
    const QUrlQuery query(requestUrl);
    QCOMPARE(requestUrl.path(), QStringLiteral("/Items/Suggestions"));
    QCOMPARE(query.queryItemValue(QStringLiteral("userId")), QStringLiteral("user-id"));
    QCOMPARE(query.queryItemValue(QStringLiteral("type")), QStringLiteral("Series"));
    QCOMPARE(query.queryItemValue(QStringLiteral("startIndex")), QStringLiteral("0"));
    QCOMPARE(query.queryItemValue(QStringLiteral("limit")), QStringLiteral("8"));
    QCOMPARE(query.queryItemValue(QStringLiteral("enableTotalRecordCount")), QStringLiteral("false"));
    QVERIFY(server.header(QByteArrayLiteral("authorization")).startsWith(QByteArrayLiteral("MediaBrowser ")));
    QCOMPARE(server.header(QByteArrayLiteral("x-emby-token")), QByteArrayLiteral("secret-token"));
}

void MediaServerSearchTest::embyDetailsPreferItemLogoArtwork()
{
    LocalMediaServer server(QByteArrayLiteral(
        R"({"Items":[{"Id":"series-1","Name":"Example Show","Type":"Series","ImageTags":{"Logo":"direct-logo-tag"},"BackdropImageTags":["backdrop-one","backdrop-two"],"ParentLogoItemId":"parent-1","ParentLogoImageTag":"parent-logo-tag"}]})"));
    QVERIFY(server.listen());
    NetworkClient networkClient;
    EmbyClient client(networkClient);
    std::optional<std::expected<MediaItem, NetworkError>> result;

    client.fetchItemDetails(sessionFor(server, ServiceType::Emby),
                            QStringLiteral("series-1"),
                            [&result](std::expected<MediaItem, NetworkError> value) {
        result = std::move(value);
    });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
    QVERIFY(result->has_value());
    const QUrl logoUrl(result->value().logoImageUrl);
    QCOMPARE(logoUrl.path(), QStringLiteral("/Items/series-1/Images/Logo"));
    QCOMPARE(QUrlQuery(logoUrl).queryItemValue(QStringLiteral("tag")), QStringLiteral("direct-logo-tag"));
    QCOMPARE(result->value().backdropImageUrls.size(), qsizetype { 2 });
    QCOMPARE(QUrl(result->value().backdropImageUrls.at(0)).path(), QStringLiteral("/Items/series-1/Images/Backdrop/0"));
    QCOMPARE(QUrl(result->value().backdropImageUrls.at(1)).path(), QStringLiteral("/Items/series-1/Images/Backdrop/1"));

    const QUrl requestUrl(QStringLiteral("http://127.0.0.1") + QString::fromLatin1(server.requestTarget()));
    QCOMPARE(QUrlQuery(requestUrl).queryItemValue(QStringLiteral("EnableImageTypes")),
             QStringLiteral("Primary,Backdrop,Logo"));
}

void MediaServerSearchTest::embyEpisodeDetailsUseInheritedSeriesLogoArtwork()
{
    LocalMediaServer server(QByteArrayLiteral(
        R"({"Items":[{"Id":"episode-1","Name":"Episode One","Type":"Episode","SeriesId":"series-1","BackdropImageTags":["episode-backdrop"],"ParentBackdropItemId":"series-1","ParentBackdropImageTags":["series-backdrop-one","series-backdrop-two"],"ParentLogoItemId":"series-1","ParentLogoImageTag":"series-logo-tag"}]})"));
    QVERIFY(server.listen());
    NetworkClient networkClient;
    EmbyClient client(networkClient);
    std::optional<std::expected<MediaItem, NetworkError>> result;

    client.fetchItemDetails(sessionFor(server, ServiceType::Emby),
                            QStringLiteral("episode-1"),
                            [&result](std::expected<MediaItem, NetworkError> value) {
        result = std::move(value);
    });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
    QVERIFY(result->has_value());
    const QUrl logoUrl(result->value().logoImageUrl);
    QCOMPARE(logoUrl.path(), QStringLiteral("/Items/series-1/Images/Logo"));
    QCOMPARE(QUrlQuery(logoUrl).queryItemValue(QStringLiteral("tag")), QStringLiteral("series-logo-tag"));
    QCOMPARE(result->value().backdropImageUrls.size(), qsizetype { 2 });
    QCOMPARE(QUrl(result->value().backdropImageUrls.at(0)).path(), QStringLiteral("/Items/series-1/Images/Backdrop/0"));
    QCOMPARE(QUrlQuery(result->value().backdropImageUrls.at(0)).queryItemValue(QStringLiteral("tag")),
             QStringLiteral("series-backdrop-one"));
}

void MediaServerSearchTest::rejectsBlankSearchTermsWithoutRequests()
{
    LocalMediaServer server;
    QVERIFY(server.listen());
    NetworkClient embyNetworkClient;
    NetworkClient jellyfinNetworkClient;
    EmbyClient embyClient(embyNetworkClient);
    JellyfinClient jellyfinClient(jellyfinNetworkClient);
    std::optional<ItemResult> embyResult;
    std::optional<ItemResult> jellyfinResult;

    embyClient.searchVideoItems(sessionFor(server, ServiceType::Emby), QStringLiteral("   "), 0, 20, [&embyResult](ItemResult value) {
        embyResult = std::move(value);
    });
    jellyfinClient.searchVideoItems(sessionFor(server, ServiceType::Jellyfin), QStringLiteral("   "), 0, 20, [&jellyfinResult](ItemResult value) {
        jellyfinResult = std::move(value);
    });

    QVERIFY(embyResult.has_value());
    QVERIFY(!embyResult->has_value());
    QCOMPARE(embyResult->error().kind, NetworkErrorKind::InvalidUrl);
    QVERIFY(jellyfinResult.has_value());
    QVERIFY(!jellyfinResult->has_value());
    QCOMPARE(jellyfinResult->error().kind, NetworkErrorKind::InvalidUrl);
    QCOMPARE(server.requestCount(), 0);
}

void MediaServerSearchTest::embyPlaybackPrefersFullDefaultSubtitleOverForcedSelection()
{
    LocalMediaServer server(QByteArrayLiteral(R"({
        "PlaySessionId":"play-session",
        "MediaSources":[{
            "Id":"source-1",
            "DefaultSubtitleStreamIndex":3,
            "MediaStreams":[
                {"Index":3,"Type":"Subtitle","IsDefault":false,"IsForced":true},
                {"Index":8,"Type":"Subtitle","IsDefault":true,"IsForced":false}
            ]
        }]
    })"));
    QVERIFY(server.listen());
    NetworkClient networkClient;
    EmbyClient client(networkClient);
    std::optional<PlaybackUrlResult> result;
    MediaItem item;
    item.id = QStringLiteral("episode-1");

    client.fetchPlaybackUrl(sessionFor(server, ServiceType::Emby), item, [&result](PlaybackUrlResult value) {
        result = std::move(value);
    });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
    QVERIFY(result->has_value());
    QCOMPARE(result->value().subtitleStreamIndex, 8);
    QCOMPARE(QUrl(result->value().url).path(), QStringLiteral("/Videos/episode-1/stream"));
    QVERIFY(!QUrlQuery(result->value().url).hasQueryItem(QStringLiteral("SubtitleStreamIndex")));
}

void MediaServerSearchTest::jellyfinPlaybackKeepsNonForcedServerSubtitleSelection()
{
    LocalMediaServer server(QByteArrayLiteral(R"({
        "playSessionId":"play-session",
        "mediaSources":[{
            "id":"source-1",
            "defaultSubtitleStreamIndex":5,
            "mediaStreams":[
                {"index":5,"type":"Subtitle","isDefault":false,"isForced":false},
                {"index":8,"type":"Subtitle","isDefault":true,"isForced":false}
            ]
        }]
    })"));
    QVERIFY(server.listen());
    NetworkClient networkClient;
    JellyfinClient client(networkClient);
    std::optional<PlaybackUrlResult> result;
    MediaItem item;
    item.id = QStringLiteral("movie-1");

    client.fetchPlaybackUrl(sessionFor(server, ServiceType::Jellyfin), item, [&result](PlaybackUrlResult value) {
        result = std::move(value);
    });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
    QVERIFY(result->has_value());
    QCOMPARE(result->value().subtitleStreamIndex, 5);
}

void MediaServerSearchTest::playbackWithoutValidSubtitleUsesPlayerAutomaticSelection()
{
    LocalMediaServer server(QByteArrayLiteral(R"({
        "PlaySessionId":"play-session",
        "MediaSources":[{
            "Id":"source-1",
            "DefaultSubtitleStreamIndex":4,
            "MediaStreams":[
                {"Index":2,"Type":"Audio","IsDefault":true},
                {"Index":4,"Type":"Subtitle","IsDefault":true,"IsExternal":true}
            ]
        }]
    })"));
    QVERIFY(server.listen());
    NetworkClient networkClient;
    EmbyClient client(networkClient);
    std::optional<PlaybackUrlResult> result;
    MediaItem item;
    item.id = QStringLiteral("movie-1");

    client.fetchPlaybackUrl(sessionFor(server, ServiceType::Emby), item, [&result](PlaybackUrlResult value) {
        result = std::move(value);
    });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
    QVERIFY(result->has_value());
    QCOMPARE(result->value().subtitleStreamIndex, -1);
}

QTEST_MAIN(MediaServerSearchTest)

#include "MediaServerSearchTest.moc"
