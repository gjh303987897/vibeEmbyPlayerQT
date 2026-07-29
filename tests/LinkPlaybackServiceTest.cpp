#include "services/link/LinkPlaybackService.h"

#include <QtTest>

class LinkPlaybackServiceTest final : public QObject {
    Q_OBJECT

private slots:
    void acceptsHttpMediaAndHlsLinks();
    void rejectsUnsupportedOrUnsafeLinks();
    void derivesDisplayNameWithoutSensitiveQueryData();
};

void LinkPlaybackServiceTest::acceptsHttpMediaAndHlsLinks()
{
    const auto media = LinkPlaybackService::resolvePlaybackUrl(
        QStringLiteral("  https://media.example.com/video/movie.mp4?token=abc%2F123#player  "));
    QVERIFY(media.has_value());
    QCOMPARE(media->scheme(), QStringLiteral("https"));
    QCOMPARE(media->host(), QStringLiteral("media.example.com"));
    QCOMPARE(media->fragment(), QString {});
    QCOMPARE(media->query(QUrl::FullyEncoded), QStringLiteral("token=abc%2F123"));

    const auto hls = LinkPlaybackService::resolvePlaybackUrl(
        QStringLiteral("http://127.0.0.1:8080/live/index.m3u8"));
    QVERIFY(hls.has_value());
    QCOMPARE(hls->path(), QStringLiteral("/live/index.m3u8"));

    const auto extensionless = LinkPlaybackService::resolvePlaybackUrl(
        QStringLiteral("https://cdn.example.com/playback?id=42"));
    QVERIFY(extensionless.has_value());
}

void LinkPlaybackServiceTest::rejectsUnsupportedOrUnsafeLinks()
{
    const auto empty = LinkPlaybackService::resolvePlaybackUrl(QString {});
    QVERIFY(!empty.has_value());
    QCOMPARE(empty.error(), LinkPlaybackError::Empty);
    QVERIFY(!LinkPlaybackService::resolvePlaybackUrl(QStringLiteral("media.example.com/video.mp4")).has_value());
    QVERIFY(!LinkPlaybackService::resolvePlaybackUrl(QStringLiteral("file:///tmp/video.mp4")).has_value());
    QVERIFY(!LinkPlaybackService::resolvePlaybackUrl(QStringLiteral("rtsp://media.example.com/live")).has_value());
    QVERIFY(!LinkPlaybackService::resolvePlaybackUrl(QStringLiteral("https:/video.mp4")).has_value());
    QVERIFY(!LinkPlaybackService::resolvePlaybackUrl(QStringLiteral("https://user:password@media.example.com/video.mp4")).has_value());
}

void LinkPlaybackServiceTest::derivesDisplayNameWithoutSensitiveQueryData()
{
    const QUrl media(QStringLiteral("https://media.example.com/path/movie.mp4?token=secret"));
    QCOMPARE(LinkPlaybackService::displayName(media), QStringLiteral("movie.mp4"));
    QCOMPARE(LinkPlaybackService::displayAddress(media),
             QStringLiteral("https://media.example.com/path/movie.mp4"));

    const QUrl streamRoot(QStringLiteral("https://live.example.com/"));
    QCOMPARE(LinkPlaybackService::displayName(streamRoot), QStringLiteral("live.example.com"));
}

QTEST_APPLESS_MAIN(LinkPlaybackServiceTest)
#include "LinkPlaybackServiceTest.moc"
