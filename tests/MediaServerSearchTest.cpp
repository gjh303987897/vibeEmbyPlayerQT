#include "network/NetworkClient.h"
#include "services/emby/EmbyClient.h"
#include "services/jellyfin/JellyfinClient.h"

#include <QHash>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QUrlQuery>

#include <optional>
#include <utility>

namespace {
class LocalMediaServer final : public QObject {
    Q_OBJECT

public:
    explicit LocalMediaServer(QObject* parent = nullptr)
        : QObject(parent)
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

                    const auto body = QByteArrayLiteral(
                        R"({"Items":[{"Id":"movie-1","Name":"Alien","Type":"Movie","ProductionYear":1979,"ImageTags":{"Primary":"poster-tag"},"UserData":{"PlayedPercentage":25.0}}],"TotalRecordCount":1})");
                    const auto response = QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: ")
                        + QByteArray::number(body.size()) + QByteArrayLiteral("\r\n\r\n") + body;
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
    void rejectsBlankSearchTermsWithoutRequests();
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

QTEST_MAIN(MediaServerSearchTest)

#include "MediaServerSearchTest.moc"
