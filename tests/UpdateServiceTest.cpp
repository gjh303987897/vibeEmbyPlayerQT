#include "services/update/UpdateService.h"

#include <QtTest>

class UpdateServiceTest final : public QObject {
    Q_OBJECT

private slots:
    void parsesStrictVersions();
    void comparesSemanticVersions();
    void filtersChannels();
    void parsesChecksumSidecar();
    void selectsHighestRelease();
};

void UpdateServiceTest::parsesStrictVersions()
{
    QVERIFY(UpdateService::parseVersion(QStringLiteral("v1.2.3")));
    QVERIFY(UpdateService::parseVersion(QStringLiteral("1.2.3-beta.1")));
    QVERIFY(!UpdateService::parseVersion(QStringLiteral("1.2")));
    QVERIFY(UpdateService::parseVersion(QStringLiteral("1.2.3-rc.1")));
    QVERIFY(!UpdateService::parseVersion(QStringLiteral("1.02.3")));
}

void UpdateServiceTest::comparesSemanticVersions()
{
    const auto alpha = *UpdateService::parseVersion(QStringLiteral("1.0.0-alpha"));
    const auto beta = *UpdateService::parseVersion(QStringLiteral("1.0.0-beta.1"));
    const auto stable = *UpdateService::parseVersion(QStringLiteral("1.0.0"));
    QVERIFY(UpdateService::compareVersions(alpha, beta) < 0);
    QVERIFY(UpdateService::compareVersions(beta, stable) < 0);
    QCOMPARE(UpdateService::compareVersions(stable, stable), 0);
}

void UpdateServiceTest::filtersChannels()
{
    const auto stable = *UpdateService::parseVersion(QStringLiteral("1.0.1"));
    const auto beta = *UpdateService::parseVersion(QStringLiteral("1.0.1-beta.2"));
    const auto alpha = *UpdateService::parseVersion(QStringLiteral("1.0.1-alpha"));
    const auto rc = *UpdateService::parseVersion(QStringLiteral("1.0.1-rc.1"));
    QVERIFY(UpdateService::channelAccepts(stable, UpdateChannel::Stable));
    QVERIFY(!UpdateService::channelAccepts(beta, UpdateChannel::Stable));
    QVERIFY(UpdateService::channelAccepts(beta, UpdateChannel::Beta));
    QVERIFY(!UpdateService::channelAccepts(alpha, UpdateChannel::Beta));
    QVERIFY(UpdateService::channelAccepts(alpha, UpdateChannel::Alpha));
    QVERIFY(!UpdateService::channelAccepts(rc, UpdateChannel::Alpha));
    QCOMPARE(UpdateService::classifyVersion(stable).value(), UpdateChannel::Stable);
    QCOMPARE(UpdateService::classifyVersion(beta).value(), UpdateChannel::Beta);
    QCOMPARE(UpdateService::classifyVersion(alpha).value(), UpdateChannel::Alpha);
    QVERIFY(!UpdateService::classifyVersion(rc));
}

void UpdateServiceTest::parsesChecksumSidecar()
{
    const auto hash = QString(64, QLatin1Char('a')).toLatin1();
    const auto fileName = QStringLiteral("vibePlayerQT-1.0.1-windows-x86_64-installer.exe");
    QCOMPARE(UpdateService::parseChecksum(hash + "  " + fileName.toUtf8(), fileName).value(), QString(64, QLatin1Char('a')));
    QVERIFY(!UpdateService::parseChecksum(hash + "  other.exe", fileName));
}

void UpdateServiceTest::selectsHighestRelease()
{
    const auto current = *UpdateService::parseVersion(QStringLiteral("1.0.0"));
    const auto json = QByteArrayLiteral(R"json([
        {"tag_name":"v1.0.1","draft":false,"body":"stable","assets":[]},
        {"tag_name":"v1.0.3","draft":true,"body":"draft","assets":[]},
        {"tag_name":"v1.0.2","draft":false,"body":"new","assets":[]}
    ])json");
    const auto result = UpdateService::parseReleases(json, UpdateChannel::Stable, current);
    QVERIFY(result);
    QCOMPARE(result->version.toString(), QStringLiteral("1.0.2"));
    QCOMPARE(result->notes, QStringLiteral("new"));
}

QTEST_MAIN(UpdateServiceTest)
#include "UpdateServiceTest.moc"
